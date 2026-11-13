#include "mikumikustudio/bone_scene_evaluation.hpp"
#include "mikumikustudio/mesh_scene_evaluation.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <d3d9.h>

namespace mikumikustudio { bool UpdateModelVertexBuffersCpu(MMDApp*, unsigned char*, const float[16]); }

// Include after bone_evaluation_host_checks.inc. Its native fresh-basis helper
// and deep source/history proofs are shared; the scene oracle below is native.
#if !MMD_ORIGINAL_PROFILE
namespace bone_scene_evaluation_probe {
namespace {
using Poses=bone_evaluation_probe::Poses;
using Inputs=std::array<BoneSceneModelInput,3>;
using Infos=std::array<BoneSceneModelInfo,3>;
using ScenePoses=std::array<Poses,3>;
using Oracle=std::array<ScenePoses,6>;
constexpr unsigned kSlots[]{2,0,1}; // Enumeration is deliberately not slot or calculation order.
constexpr int kFrames[]{0,5,10,15,20,25};
bool SameId(const BoneSceneIdentity& a,const BoneSceneIdentity& b) {
    return a.lo==b.lo&&a.hi==b.hi&&a.generation==b.generation;
}
Inputs PublishInputs(MMDApp* app) {
    using namespace mikumikustudio::plugins;
    auto store=CreateSceneStore(app);SceneClient client;auto api=([&]{ MmsSceneApiV1 value{}; if (store->Api(client, value) != MMS_OK) throw std::runtime_error("scene API binding failed"); return value; }());
    MmsSceneQueryV1 q{};q.h=Header<MmsSceneQueryV1>();q.stage=MMS_STAGE_BIND;q.flags=MMS_SCENE_CURRENT_TIME;q.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 bind{};Check(api.acquire(api.context,&q,&bind)==MMS_OK,"publish real host scene identities");
    MmsSpan objects{};Check(api.objects(api.context,bind.handle,&objects)==MMS_OK&&objects.count==3,"three actual model identities available");
    const auto* rows=static_cast<const MmsObjectV1*>(objects.data);Inputs result{};
    // BIND enumerates model slots ascending (scene_host.cpp), before accessories.
    for (unsigned i=0;i<3;++i) {
        const auto slot=kSlots[i];const auto& object=rows[slot].object;
        Check(rows[slot].kind==MMS_OBJECT_MODEL,"BIND identity row is a loaded model");
        result[i]={slot,{object.id.lo,object.id.hi,object.generation},app->ModelSlot(slot)};
    }
    api.release(api.context,bind.handle);return result;
}
Infos Describe(const Inputs& inputs) {
    Infos result{};for(unsigned i=0;i<3;++i) {const auto& m=*mdl::Mdl(inputs[i].model);
        result[i]={inputs[i].slot,m.comboSelIndex2,m.boneCount,inputs[i].identity};}return result;
}
bool InfoMatches(const BoneSceneModelInfo& a,const BoneSceneModelInfo& b) {
    return a.slot==b.slot&&a.calculationOrder==b.calculationOrder&&a.boneCount==b.boneCount&&SameId(a.identity,b.identity);
}
ScenePoses NativeAt(MMDApp* app,int frame,bool wrongPerModel=false) {
    ScenePoses result;std::array<unsigned,3> order{0,1,2};
    for(unsigned slot=0;slot<3;++slot) {auto* raw=app->ModelSlot(slot);bone_evaluation_probe::Fresh(raw);SeekModelFrame(raw,frame,0);}
    std::sort(order.begin(),order.end(),[&](unsigned a,unsigned b){
        const auto aa=mdl::Mdl(app->ModelSlot(a))->comboSelIndex2,bb=mdl::Mdl(app->ModelSlot(b))->comboSelIndex2;
        return aa!=bb?aa<bb:a<b;});
    for(unsigned i=0;i<3;++i) result[i].resize(mdl::Mdl(app->ModelSlot(kSlots[i]))->boneCount);
    const auto phase=[&](unsigned slot,int p) {
        auto* raw=app->ModelSlot(slot);auto& model=*mdl::Mdl(raw);ModelApplyMorphs(raw);SetPhysicsMode(raw,p,app->ModelSlots(),0);
        unsigned row=0;while(kSlots[row]!=slot)++row;
        for(uint32_t b=0;b<model.boneCount;++b) std::copy(model.boneTable[b].matExtra,model.boneTable[b].matExtra+16,result[row][b].attachment[p].begin());
    };
    if(wrongPerModel) for(auto slot:order) for(int p=0;p<2;++p) phase(slot,p);
    else for(int p=0;p<2;++p) for(auto slot:order) phase(slot,p);
    for(unsigned i=0;i<3;++i) {
        auto* raw=app->ModelSlot(kSlots[i]);auto& model=*mdl::Mdl(raw);
        for(uint32_t b=0;b<model.boneCount;++b) std::copy(model.boneTable[b].matInit,model.boneTable[b].matInit+16,result[i][b].rigid.begin());
        bone_scale::ApplyTransforms(raw);
        for(uint32_t b=0;b<model.boneCount;++b) {const auto& bone=model.boneTable[b];auto& out=result[i][b];
            std::copy(bone.matInit,bone.matInit+16,out.scaled.begin());
            for(unsigned c=0;c<3;++c) out.jointPosition[c]=bone.position[0]*bone.matInit[c]+bone.position[1]*bone.matInit[c+4]+bone.position[2]*bone.matInit[c+8]+bone.matInit[c+12];
        }
    }
    return result;
}
Oracle Native(MMDApp* app) {Oracle out;for(unsigned t=0;t<6;++t) out[t]=NativeAt(app,kFrames[t]);return out;}
// Test-owned typed COM buffers let the real native CPU worker run without
// reading from, replacing or requiring a GPU-managed vertex buffer.
struct CpuBuffer final : IDirect3DVertexBuffer9 {
    std::vector<float> values; bool locked=false;
    explicit CpuBuffer(size_t count):values(count,-999.f){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {if(out)*out=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return 2;}
    ULONG STDMETHODCALLTYPE Release() override{return 1;}
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override{return E_NOTIMPL;}
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override{return 0;}
    DWORD STDMETHODCALLTYPE GetPriority() override{return 0;}
    void STDMETHODCALLTYPE PreLoad() override{}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override{return D3DRTYPE_VERTEXBUFFER;}
    HRESULT STDMETHODCALLTYPE Lock(UINT offset,UINT size,void** out,DWORD flags) override {
        if(locked||!out||offset||size!=values.size()*sizeof(float)||flags)return E_FAIL;
        locked=true;*out=values.data();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override{if(!locked)return E_FAIL;locked=false;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC*) override{return E_NOTIMPL;}
};
struct NativeMesh { std::vector<float> values; uint32_t count=0,uvCount=0; };
using SceneMeshes=std::array<NativeMesh,3>;
SceneMeshes NativeMeshes(MMDApp* app) {
    SceneMeshes result;const float world[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(unsigned row=0;row<3;++row) {
        auto* raw=app->ModelSlot(kSlots[row]);auto& model=*mdl::Mdl(raw);
        auto& out=result[row];out.count=model.vertexCount;out.uvCount=model.pmxAdditionalUvCount;
        Check(model.physicsMode==2 && out.count>0 && out.uvCount<=4,"native three-model fixture has actual PMX vertices and valid UV stride");
        const auto stride=8u+4u*out.uvCount;
        CpuBuffer body(size_t(out.count)*stride),edge(size_t(out.count)*4);
        struct RestoreBuffers {
            mdl::ModelRecord& model;void* body;void* edge;
            ~RestoreBuffers(){model.vertexBuffer=body;model.vertexBuffer2=edge;}
        } restore{model,model.vertexBuffer,model.vertexBuffer2};
        model.vertexBuffer=&body;model.vertexBuffer2=&edge;
        Check(UpdateModelVertexBuffersCpu(app,raw,world),"real CPU skinning fills private typed buffers for loaded model");
        Check(!body.locked&&!edge.locked,"native skinning unlocks its test buffers");
        out.values=std::move(body.values);
        model_vertex_cache::Forget(raw);
    }
    return result;
}
bool MeshFrameMatches(const MeshSceneFrame& actual,const SceneMeshes& expected) {
    if(actual.ModelCount()!=3)return false;
    for(unsigned row=0;row<3;++row) {
        const auto* frame=actual.ModelFrame(row);const auto& src=expected[row];
        if(!frame||frame->VertexCount()!=src.count||frame->AdditionalUvCount()!=src.uvCount)return false;
        const auto stride=8u+4u*src.uvCount;
        for(uint32_t i=0;i<src.count;++i) {
            MeshEvaluationVertex vertex{};if(!frame->VertexAt(i,vertex))return false;
            const float* native=src.values.data()+size_t(i)*stride;
            if(std::memcmp(vertex.position.data(),native,12)||std::memcmp(vertex.normal.data(),native+3,12)||std::memcmp(vertex.uv.data(),native+6,8))return false;
            for(unsigned u=0;u<src.uvCount;++u)if(std::memcmp(vertex.additionalUv[u].data(),native+8+u*4,16))return false;
        }
    }
    return true;
}
bool FrameMatches(const BoneSceneFrame& actual,const ScenePoses& wanted,const Infos& info,double time) {
    if(actual.ModelCount()!=3||actual.Frame()!=time)return false;
    for(unsigned i=0;i<3;++i) {BoneSceneModelInfo got{};const auto* child=actual.ModelFrame(i);
        if(!actual.ModelAt(i,got)||!InfoMatches(got,info[i])||!child||!bone_evaluation_probe::FrameMatches(*child,wanted[i],time))return false;}
    return actual.ModelFrame(3)==nullptr;
}
bool Matches(const BoneSceneSource& source,const BoneTransformContext& context,const Oracle& wanted,const Infos& info) {
    if(source.ModelCount()!=3)return false;
    for(unsigned i=0;i<3;++i) {BoneSceneModelInfo got{};if(!source.ModelAt(i,got)||!InfoMatches(got,info[i]))return false;}
    for(unsigned t=0;t<6;++t) {auto frame=source.Evaluate(kFrames[t],context);if(!frame||!FrameMatches(*frame,wanted[t],info,kFrames[t]))return false;}
    return true;
}
void ExactBeforeParentKey(const BoneSceneSource& source,const BoneTransformContext& context,const Infos& info) {
    EvaluationTime time;
    Check(EvaluationTime::FromSeconds(36028797018963968LL,270215977642229761ULL,time)&&time.Frame()==4&&time.DiscreteFrame()==3,
        "rational time before parent key retains exact side despite rounding to frame four");
    const auto before=source.Evaluate(std::nextafter(4.0,0.0),context),exact=source.EvaluateAt(time,context),rounded=source.Evaluate(4,context);
    ScenePoses expected,after;
    for(unsigned i=0;i<3;++i){
        const auto* a=before->ModelFrame(i);const auto* b=rounded->ModelFrame(i);Check(a&&b,"real parent boundary frames own all models");
        expected[i].resize(a->BoneCount());after[i].resize(b->BoneCount());
        for(unsigned bone=0;bone<a->BoneCount();++bone)Check(a->BoneAt(bone,expected[i][bone])&&b->BoneAt(bone,after[i][bone]),"read boundary comparison poses");
        Check(exact->ModelFrame(i)->Visibility()==a->Visibility(),"exact scene time keeps pre-key visibility");
    }
    Check(FrameMatches(*exact,expected,info,4),"exact time keeps all pre-key transforms without quantizing continuous pose time");
    Check(bone_evaluation_probe::Different(expected[1],after[1]),"real consumer detects erroneous early external-parent switch");
}
void Workers(const BoneSceneSource& old,const BoneSceneSource& newer,const BoneTransformContext& context,
    const Oracle& before,const Oracle& after,const Infos& info) {
    bool a=true,b=true;const auto read=[&](bool& ok){try {for(unsigned n=0;n<4;++n)ok=Matches(old,context,before,info)&&Matches(newer,context,after,info)&&ok;}catch(...){ok=false;}};
    std::thread first([&]{read(a);}),second;try{second=std::thread([&]{read(b);});}catch(...){first.join();throw;}
    first.join();second.join();Check(a&&b,"parallel frozen scene samples retain complete old and new target motions");
}
struct Proof {
    std::array<std::unique_ptr<bone_evaluation_probe::Proof>,3> models;
    std::array<std::vector<unsigned char>,3> payloads;
    MMDApp* app;
    static std::vector<unsigned char> DisplayBytes(const mdl::ModelRecord& m) {
        std::vector<unsigned char> out;for(unsigned k=0;k<3;++k) {const auto& key=m.displayKeys[k];
            const auto* p=reinterpret_cast<const unsigned char*>(key.selectorStates);out.insert(out.end(),p,p+m.boneOrderCount*sizeof(mdl::BoneReference));
            if(m.ikChainCount)out.insert(out.end(),key.ikStates,key.ikStates+m.ikChainCount);}return out;
    }
    explicit Proof(MMDApp* value):app(value) {for(unsigned i=0;i<3;++i){models[i]=std::make_unique<bone_evaluation_probe::Proof>(app,app->ModelSlot(i));payloads[i]=DisplayBytes(*mdl::Mdl(app->ModelSlot(i)));}}
    void Verify()const{for(unsigned i=0;i<3;++i){models[i]->Verify();Check(payloads[i]==DisplayBytes(*mdl::Mdl(app->ModelSlot(i))),"scene capture preserves every nested display key payload");}}
};
void Seed(MMDApp* app) {
    for(unsigned slot=0;slot<3;++slot) {
        auto* raw=app->ModelSlot(slot);bone_evaluation_probe::Seed(raw);auto& m=*mdl::Mdl(raw);
        m.comboSelIndex2=slot==1?1:slot==0?2:3;
        Check(mdl::DisplayKeyPoolEnsure(m,3),"reserve three real external-parent display keys");
        for(unsigned k=0;k<3;++k) {auto& key=m.displayKeys[k];Check(key.selectorStates&&key.ikStates,"real nested display payloads allocated");
            key.frame=k==0?0:k==1?4:20;key.previous=k?k-1:0;key.next=k<2?k+1:0;key.visible=1;key.allocated=0;
            for(uint32_t i=0;i<m.ikChainCount;++i)key.ikStates[i]=1;
            for(uint32_t i=0;i<m.boneOrderCount;++i)key.selectorStates[i]={-1,0};
            if(k==1&&slot==0){key.selectorStates[0]={1,1};const auto local=m.boneTable[0].slotIndex;Check(local>0,"consumer has local Move selector");key.selectorStates[local]={1,0};}
            if(k==1&&slot==2)key.selectorStates[0]={0,4};
        }
    }
}
}
void Run(MMDApp* app) {
    wchar_t path[32768]{},target[32768]{};
    const auto a=GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768),b=GetEnvironmentVariableW(L"MMS_SCENE_TEST_TARGET_MODEL",target,32768);
    Check(a&&a<32768&&b&&b<32768,"consumer and phase-sensitive target fixture supplied");
    const auto load=[&]{LoadModelFile(app,path,false);LoadModelFile(app,target,false);LoadModelFile(app,path,false);Check(app->ModelSlot(0)&&app->ModelSlot(1)&&app->ModelSlot(2),"load actual three-model dependency chain");Seed(app);};load();
    auto& targetModel=*mdl::Mdl(app->ModelSlot(1));Check((targetModel.boneTable[3].flags&0x1000)&&!(targetModel.boneTable[1].flags&0x1000),"target after-physics IK controller drives a before-physics link");
    auto inputs=PublishInputs(app);const auto info=Describe(inputs);auto context=BoneTransformContext::Create();Check(bool(context),"create private scene CPU mathematics");
    const auto original=Native(app);const auto wrong=NativeAt(app,10,true);
    Check(!bone_evaluation_probe::Same(original[2][2][1].attachment[0],original[2][2][1].attachment[1]),"actual target link attachment differs between scene phases");
    Check(bone_evaluation_probe::Different(original[2][1],wrong[1]),"fixture detects incorrect per-model two-phase ordering in consumer");
    const auto key=targetModel.boneCount+3*2;const auto saved=targetModel.boneKeys[key];targetModel.boneKeys[key].position[0]+=1.75f;
    const auto edited=Native(app);targetModel.boneKeys[key]=saved;
    Check(bone_evaluation_probe::Different(original[2][1],edited[2][1])&&bone_evaluation_probe::Different(original[2][0],edited[2][0]),"changing only target motion affects consumer and transitive dependent");
    // Bind one existing vertex in each dependent to a bone that actually
    // changes. The original fixture's active skinning weights need not name
    // any affected bone, so pose differences alone cannot prove deformation.
    for(unsigned row:{0u,1u}) {
        auto& model=*mdl::Mdl(app->ModelSlot(kSlots[row]));uint32_t chosen=model.boneCount;
        for(uint32_t bone=0;bone<model.boneCount;++bone)
            if(original[2][row][bone].scaled!=edited[2][row][bone].scaled){chosen=bone;break;}
        Check(chosen<model.boneCount&&model.vertexCount>0,"dependent has a changing bone and a vertex");
        model.pmxVertices[0].weightType=mdl::PmxWeightType::bdef1;
        model.pmxVertices[0].bone[0]=int32_t(chosen);
    }
    NativeAt(app,10);const auto originalMeshes=NativeMeshes(app);
    targetModel.boneKeys[key].position[0]+=1.75f;
    NativeAt(app,10);const auto editedMeshes=NativeMeshes(app);targetModel.boneKeys[key]=saved;
    Check(originalMeshes[1].values!=editedMeshes[1].values&&originalMeshes[0].values!=editedMeshes[0].values,
        "real target motion changes consumer and transitive dependent skinned vertices");
    for(unsigned slot=0;slot<3;++slot){bone_scale::RestoreTransforms(app->ModelSlot(slot));mdl::Mdl(app->ModelSlot(slot))->boneTable[0].trans[0]=123;}
    app->CurrentFrame()=0;app->SceneModified()=0;
    // Current time is inactive: nevertheless every future reference is validated.
    auto& future=mdl::Mdl(app->ModelSlot(0))->displayKeys[2].selectorStates[0];const auto futureSaved=future;
    for(const auto invalid:{mdl::BoneReference{99,0},mdl::BoneReference{1,100000}}){future=invalid;bool rejected=false;
        {const Proof proof(app);try{auto unused=BoneSceneSource::Capture(inputs.data(),3);}catch(const std::invalid_argument&){rejected=true;}proof.Verify();}
        Check(rejected,"future missing slot or invalid target bone rejects scene capture atomically");}future=futureSaved;
    std::unique_ptr<BoneSceneSource> old,newer;std::unique_ptr<MeshSceneSource> oldMeshes,newMeshes;
    EvaluationTime meshTime;Check(EvaluationTime::FromFrame(10,meshTime),"independent mesh comparison time is valid");
    {const Proof proof(app);old=BoneSceneSource::Capture(inputs.data(),3);Check(old&&Matches(*old,*context,original,info),"captured scene matches real native phase and model order");ExactBeforeParentKey(*old,*context,info);
        oldMeshes=MeshSceneSource::Capture(inputs.data(),3);auto frame=oldMeshes->EvaluateAt(meshTime,*context);
        Check(FrameMatches(frame->Bones(),original[2],info,10)&&MeshFrameMatches(*frame,originalMeshes),"three frozen models match native phase order and actual CPU skinned vertices");proof.Verify();}
    targetModel.boneKeys[key].position[0]+=1.75f;
    {const Proof proof(app);newer=BoneSceneSource::Capture(inputs.data(),3);Workers(*old,*newer,*context,original,edited,info);
        newMeshes=MeshSceneSource::Capture(inputs.data(),3);auto after=newMeshes->EvaluateAt(meshTime,*context);auto before=oldMeshes->EvaluateAt(meshTime,*context);
        Check(FrameMatches(after->Bones(),edited[2],info,10)&&MeshFrameMatches(*after,editedMeshes),"edited target yields native consumer and transitive skinned geometry");
        Check(FrameMatches(before->Bones(),original[2],info,10)&&MeshFrameMatches(*before,originalMeshes),"old mesh scene retains original target motion");proof.Verify();}
    targetModel.boneKeys[key]=saved;
    auto retainedOld=old->Evaluate(10,*context),retainedNew=newer->Evaluate(10,*context);
    NativeAt(app,0);for(unsigned slot=0;slot<3;++slot)bone_scale::RestoreTransforms(app->ModelSlot(slot));app->CurrentFrame()=0;
    const auto project=std::filesystem::absolute(L"bone-scene-evaluation.pmm");Check(SaveSceneCopy(app,project.c_str()),"save real three-model PMM with registered external-parent windows");
    ResetAppState(app);Workers(*old,*newer,*context,original,edited,info);ExactBeforeParentKey(*old,*context,info);
    {auto before=oldMeshes->EvaluateAt(meshTime,*context);auto after=newMeshes->EvaluateAt(meshTime,*context);
        Check(MeshFrameMatches(*before,originalMeshes)&&MeshFrameMatches(*after,editedMeshes),"mesh scenes retain original and edited geometry after all source models are deleted");}
    load();auto replacementInputs=PublishInputs(app);const auto replacementInfo=Describe(replacementInputs);
    Check(!SameId(info[2].identity,replacementInfo[2].identity),"reused target slot receives a different real host identity");
    {const Proof proof(app);auto replacement=BoneSceneSource::Capture(replacementInputs.data(),3);Check(replacement&&Matches(*replacement,*context,original,replacementInfo),"new scene capture uses replacement identities");Workers(*old,*newer,*context,original,edited,info);proof.Verify();}
    ResetAppState(app);wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();Check(app->ModelSlot(0)&&app->ModelSlot(1)&&app->ModelSlot(2),"reload actual three-model PMM");
    auto reloadInputs=PublishInputs(app);const auto reloadInfo=Describe(reloadInputs);
    {const Proof proof(app);auto restored=BoneSceneSource::Capture(reloadInputs.data(),3);Check(restored&&Matches(*restored,*context,original,reloadInfo),"PMM reload preserves native dependency motion and phase ordering");Workers(*old,*newer,*context,original,edited,info);proof.Verify();}
    auto retainedMesh=oldMeshes->EvaluateAt(meshTime,*context);
    ResetAppState(app);old.reset();newer.reset();oldMeshes.reset();newMeshes.reset();context.reset();bool retained=false;
    std::thread reader([&]{retained=FrameMatches(*retainedOld,original[2],info,10)&&FrameMatches(*retainedNew,edited[2],info,10);});reader.join();
    Check(retained,"owned scene frames retain identities and poses after all models sources and math context are destroyed");
    Check(MeshFrameMatches(*retainedMesh,originalMeshes),"owned three-model mesh frame survives source models and math context");
    std::puts("Frozen bone scene: actual three-model two-phase native parity, frozen target motion, identities and lifetime passed");
}
}
#endif
