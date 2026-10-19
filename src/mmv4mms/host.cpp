#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include "btBulletDynamicsCommon.h"
#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_mesh.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/shared_toon.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/transparent_capture.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mmv/mesh.hpp"
#include "mmv/mms.h"
#include "mmv/menu.h"
#include "pmx_optional_fields.hpp"
#include "material_pass.hpp"
extern "C" float* ExpGetAcsWorldMat(float*,int);
namespace mikumikustudio {float ModelOutlineDistance(MMDApp*,unsigned char*,const float[16]);}
namespace mikumikustudio::mmv4mms {
namespace {
using Microsoft::WRL::ComPtr;
MMDApp* host=nullptr;
HMODULE library=nullptr;
MmvMmsApi api{};
MmvMenuApi menuApi{};
void* plugin=nullptr;
bool attempted=false;
uint64_t generation=0,frameNumber=0;
std::unordered_map<const void*,uint64_t> generations;
std::filesystem::path directory,openedProject;
ComPtr<IDirect3DTexture9> rendererColor;
std::string lastError;
void Check(HRESULT hr,const char* action){if(FAILED(hr))throw std::runtime_error(action);}
uint64_t Generation(const void* p){auto& g=generations[p];if(!g)g=++generation;return g;}
void EffectiveWeights(uint32_t type,const int bones[4],const float weights[4],int outBones[4],float outWeights[4]){native_mesh::EffectiveWeights(type,bones,weights,outBones,outWeights);}
std::string Utf8(const wchar_t* text) {
    if(!text||!*text)return {};
    const int n=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    std::string s(size_t(n),0);WideCharToMultiByte(CP_UTF8,0,text,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;
}
std::string Name(const wchar_t* wide,const char* sjis,size_t bound=20) {
    if(wide&&*wide)return Utf8(wide);
    const auto count=static_cast<int>(strnlen_s(sjis,bound));
    std::wstring w(count,L'\0');const int n=MultiByteToWideChar(932,0,sjis,count,w.data(),count);w.resize(n);return Utf8(w.c_str());
}
void Matrix(const btTransform& t,float* out) {
    mmv::Identity(out);const auto& b=t.getBasis();
    for(int row=0;row<3;++row)for(int col=0;col<3;++col)out[row*4+col]=float(b[col][row]);
    for(int i=0;i<3;++i)out[12+i]=float(t.getOrigin()[i]);
}
#include "physics_input.inl"
struct Snapshot {
    MmvObject object{};
    MmvObjectRenderData renderData{};
    std::wstring path;
    std::deque<std::string> strings;
    mmv::Mesh mesh;
    uint32_t originalCount=0;
    std::vector<MmvVertex> original;
    std::vector<MmvWeight> weights;
    std::vector<MmvBone> bones;
    std::vector<MmvMorph> morphs;
    std::vector<MmvRigidBody> rigids;
    std::vector<MmvJoint> joints;
    const char* Keep(std::string s){strings.push_back(std::move(s));return strings.back().c_str();}
    MmvObject View() {
        object.mesh=mesh.View();object.path=path.c_str();object.originalVertices=original.data();object.weights=weights.data();
        object.originalVertexCount=originalCount?originalCount:uint32_t(original.size());
        object.bones=bones.data();object.boneCount=uint32_t(bones.size());object.morphs=morphs.data();object.morphCount=uint32_t(morphs.size());
        object.rigids=rigids.data();object.rigidCount=uint32_t(rigids.size());object.joints=joints.data();object.jointCount=uint32_t(joints.size());return object;
    }
};
void ReadVertices(IDirect3DVertexBuffer9* vb,uint32_t count,uint32_t stride,std::vector<MmvVertex>& out) {
    // Bone-only models are valid and do not allocate a vertex buffer.
    if(!count){out.clear();return;}
    if(!vb)throw std::runtime_error("Missing source vertex buffer");
    D3DVERTEXBUFFER_DESC desc{};Check(vb->GetDesc(&desc),"Get vertex buffer description");
    if(uint64_t(count)*stride>desc.Size||stride<32||stride>96)throw std::runtime_error("Invalid source vertex range");
    void* data=nullptr;Check(vb->Lock(0,0,&data,D3DLOCK_READONLY),"Read source vertices");
    out.resize(count);
    for(uint32_t i=0;i<count;++i){auto& v=out[i];memcpy(v.position,static_cast<char*>(data)+size_t(i)*stride,stride);v.sourceVertex=i;v.edgeScale=1;}
    vb->Unlock();
}
void ReadTriangles(IDirect3DIndexBuffer9* ib,uint32_t count,std::vector<MmvTriangle>& out) {
    if(!count){out.clear();return;}
    if(!ib)throw std::runtime_error("Missing source index buffer");
    D3DINDEXBUFFER_DESC d{};Check(ib->GetDesc(&d),"Get index buffer description");
    const uint32_t stride=d.Format==D3DFMT_INDEX32?4:2;
    if(count%3||uint64_t(count)*stride>d.Size)throw std::runtime_error("Invalid source index range");
    void* data=nullptr;Check(ib->Lock(0,0,&data,D3DLOCK_READONLY),"Read source indices");out.resize(count/3);
    for(uint32_t i=0;i<count;++i)out[i/3].vertex[i%3]=stride==4?static_cast<uint32_t*>(data)[i]:static_cast<uint16_t*>(data)[i];
    ib->Unlock();
}
// Native pose matrices are affine. Inverse/multiply operations may leave
// float round-off in the homogeneous column; Bullet's input contract uses
// exact 0/1 values. Canonicalize only that round-off, preserving genuinely
// invalid matrices for the deformer to diagnose.
void CanonicalizeBoneAffine(float matrix[16]) {
    constexpr float tolerance=1.0e-5f;
    if(!std::isfinite(matrix[3])||!std::isfinite(matrix[7])||
       !std::isfinite(matrix[11])||!std::isfinite(matrix[15]))return;
    if(std::fabs(matrix[3])<=tolerance&&std::fabs(matrix[7])<=tolerance&&
       std::fabs(matrix[11])<=tolerance&&std::fabs(matrix[15]-1.f)<=tolerance){
        matrix[3]=matrix[7]=matrix[11]=0.f;matrix[15]=1.f;
    }
}
std::unique_ptr<Snapshot> ModelSnapshot(unsigned char* source,uint32_t instance,const float world[16],uint32_t demand) {
    auto* renderSource=native_mesh::RenderModel(source);auto& rendered=*mdl::Mdl(renderSource);auto& m=*mdl::Mdl(source);auto s=std::make_unique<Snapshot>();auto& o=s->object;
    o.structureSize=sizeof(o);o.kind=MMV_MODEL;o.instance=instance;o.visible=m.loadComplete!=0;
    o.id=reinterpret_cast<uintptr_t>(source);o.generation=Generation(source);s->path=m.path;
    o.name=s->Keep(Utf8(text_encoding::ModelName(m,false).c_str()));mmv::Identity(o.world);
    const bool pmx=m.physicsMode==2;const uint32_t uv=pmx?m.pmxAdditionalUvCount:0;
    s->renderData.outlineDistance=ModelOutlineDistance(host,source,world);
    s->renderData.flags=pmx?MMV_OBJECT_RENDER_PMX:0u;
    if(!m.toonFlag)s->renderData.flags|=MMV_OBJECT_RENDER_NO_SELF_SHADOW;
    if(plugin){if(host->state.characterTransparentMode)s->renderData.flags|=::mmv::kObjectTransparentMode;if(host->state.selfShadowEnabled)s->renderData.flags|=::mmv::kObjectOpaqueEdgeColor;}
    if(uv>4)throw std::runtime_error("Invalid additional UV count");
    const bool needVertices=(demand&MMV_INPUT_DEMAND_VERTICES)!=0;
    const bool needTriangles=(demand&MMV_INPUT_DEMAND_TRIANGLES)!=0;
    const bool needMaterials=(demand&MMV_INPUT_DEMAND_MATERIALS)!=0;
    const bool needOriginal=(demand&MMV_INPUT_DEMAND_ORIGINAL_VERTICES)!=0;
    const bool needWeights=(demand&MMV_INPUT_DEMAND_WEIGHTS)!=0;
    if(needVertices) {
        ReadVertices(static_cast<IDirect3DVertexBuffer9*>(rendered.vertexBuffer),rendered.vertexCount,32+16*uv,s->mesh.vertices);
    }
    if(needTriangles){
        if(rendered.indexCount%3||(rendered.indexCount&&!rendered.indices))throw std::runtime_error("Invalid source topology");
        s->mesh.triangles.resize(rendered.indexCount/3);
        // PMD's CPU index table is WORD[2*count], with the high words zero.
        for(uint32_t i=0;i<rendered.indexCount;++i)s->mesh.triangles[i/3].vertex[i%3]=renderSource!=source?uint32_t(native_mesh::Indices32(renderSource)[i]):pmx?static_cast<const uint32_t*>(rendered.indices)[i]:static_cast<const uint16_t*>(rendered.indices)[2*i];
    }
    if(needOriginal||needWeights)s->originalCount=m.vertexCount;
    if(needOriginal)s->original.resize(m.vertexCount);
    if(needWeights)s->weights.resize(m.vertexCount);
    if(needOriginal||needWeights)for(uint32_t i=0;i<m.vertexCount;++i){
        auto* v=needOriginal?&s->original[i]:nullptr;auto* w=needWeights?&s->weights[i]:nullptr;
        if(v)v->sourceVertex=i;
        if(pmx){const auto& p=m.pmxVertices[i];MmvVertex ignoredVertex{};MmvWeight ignoredWeight{};
            if(v){memcpy(v->position,p.position,32);v->edgeScale=p.edgeScale;v->flags=p.hasPmxEdgeData?0:MMV_VERTEX_EDGE_DISABLED;}
            CopyPmxOptionalFields(p,uv,v?*v:ignoredVertex,w?*w:ignoredWeight);
            if(w){w->type=uint32_t(p.weightType);EffectiveWeights(w->type,p.bone,p.weight,w->bones,w->weights);}
        }else{const auto& p=m.rawVertices[i];
            if(v){memcpy(v->position,p.position,32);v->edgeScale=p.edgeDisabled?0.f:1.f;v->flags=p.edgeDisabled?MMV_VERTEX_EDGE_DISABLED:0;}
            if(w){w->bones[0]=p.bone[0];w->bones[1]=p.bone[1];w->bones[2]=w->bones[3]=-1;w->weights[0]=p.weightPercent*.01f;w->weights[1]=1-w->weights[0];w->type=1;}
        }
        if(needVertices&&v&&i<s->mesh.vertices.size()){s->mesh.vertices[i].edgeScale=v->edgeScale;s->mesh.vertices[i].flags=v->flags;}
    }
    // Edge data belongs to the posed stream even when no bind-pose snapshot
    // was requested by a scene-only renderer.
    if(needVertices&&!needOriginal)for(size_t i=0;i<std::min(size_t(m.vertexCount),s->mesh.vertices.size());++i){
        auto& v=s->mesh.vertices[i];
        if(pmx){v.edgeScale=m.pmxVertices[i].edgeScale;v.flags=m.pmxVertices[i].hasPmxEdgeData?0:MMV_VERTEX_EDGE_DISABLED;}
        else{v.edgeScale=m.rawVertices[i].edgeDisabled?0.f:1.f;v.flags=m.rawVertices[i].edgeDisabled?MMV_VERTEX_EDGE_DISABLED:0;}
    }
    // Restore bind-pose components in our copy; MMS keeps its current morphs.
    if(needOriginal){
        if(const auto* base=mdl::BaseVertexMorphTable(source))for(uint32_t i=0;i<mdl::BaseVertexMorphCount(source);++i)
            if(base[i].vertexIndex<m.vertexCount)memcpy(s->original[base[i].vertexIndex].position,base[i].offset,12);
        if(pmx)for(int family=0;family<5;++family){const auto* table=mdl::UvMorphTables(source).byFamily[family];const auto count=mdl::UvMorphCounts(source).byFamily[family];
            if(table)for(int i=0;i<count;++i)if(table[i].vertexIndex>=0&&uint32_t(table[i].vertexIndex)<m.vertexCount){auto& v=s->original[table[i].vertexIndex];
                if(family==0)memcpy(v.uv,table[i].offset,8);else memcpy(v.additionalUv+(family-1)*4,table[i].offset,16);}}
    }
    s->mesh.materials.resize(rendered.materialCount);uint32_t first=0;
    for(uint32_t i=0;i<rendered.materialCount;++i){const auto& a=mdl::Materials(renderSource)[i];auto& b=s->mesh.materials[i];
        b.key="source:"+std::to_string(i);b.name="subset"+std::to_string(i);b.value.sourceMaterial=i;
        const auto localName=text_encoding::MaterialName(rendered,i,false),englishName=text_encoding::MaterialName(rendered,i,true);
        if(!localName.empty())b.name=Utf8(localName.c_str());
        b.nameEn=englishName.empty()?b.name:Utf8(englishName.c_str());
        if(needMaterials){
            memcpy(b.value.diffuse,a.diffuse,16);memcpy(b.value.ambient,a.ambient,12);memcpy(b.value.specular,a.specular,12);b.value.power=a.specularPower;
            if(plugin&&m.displayState)b.value.diffuse[3]=.5f;
            memcpy(b.value.edgeColor,a.edgeColor,16);b.value.edgeSize=a.edgeSize;b.value.flags=pmx?a.flags:(a.doubleSided?1u:0u);b.value.sphereMode=a.sphereMode;
            b.texture.path=a.texturePath;b.sphere.path=a.spherePath;b.toon.path=a.toonPath;
        }
        if(needTriangles){
            if(a.faceVertexCount<0||a.faceVertexCount%3||uint64_t(first)+a.faceVertexCount/3>s->mesh.triangles.size())throw std::runtime_error("Invalid source material range");
            for(uint32_t j=0;j<uint32_t(a.faceVertexCount)/3;++j)s->mesh.triangles[first++].material=i;
        }
    }
    s->bones.resize(m.boneCount);
    for(uint32_t i=0;i<m.boneCount;++i){const auto& a=m.boneTable[i];auto& b=s->bones[i];b.name=s->Keep(Utf8(text_encoding::BoneName(a,false).c_str()));
        b.parent=a.parent;memcpy(b.origin,a.position,12);
        if(demand&MMV_INPUT_DEMAND_BONES){memcpy(b.world,a.matInit,64);for(int c=0;c<3;++c)b.world[12+c]+=a.position[0]*a.matInit[c]+a.position[1]*a.matInit[4+c]+a.position[2]*a.matInit[8+c];CanonicalizeBoneAffine(b.world);b.valid=1;}}
    s->morphs.resize(m.morphCount);for(uint32_t i=0;i<m.morphCount;++i){const auto& a=m.morphs[i];s->morphs[i]={s->Keep(Utf8(text_encoding::MorphName(a,false).c_str())),(demand&MMV_INPUT_DEMAND_MORPHS)?a.value:0.f,(demand&MMV_INPUT_DEMAND_MORPHS)?a.type:0u};}
    if(demand&MMV_INPUT_DEMAND_RIGIDS)s->rigids.resize(m.rigidCount);
    // Native keyframes temporarily set mode=0 when a bone disables native
    // physics. External solvers need the model's original PMX/PMD body mode.
    if(demand&MMV_INPUT_DEMAND_RIGIDS)for(uint32_t i=0;i<m.rigidCount;++i){const auto& a=m.rigidTable[i];auto& b=s->rigids[i];b.name=s->Keep(Name(a.jpText,a.name));b.shape=a.shape;b.mode=a.staticFlag?0u:(a.kinematicFlag?2u:1u);b.group=a.group;b.collisionMask=a.noCollapse;b.bone=a.boneIndex;
        memcpy(b.size,a.size,12);b.mass=a.mass;b.damping[0]=a.linearDamping;b.damping[1]=a.angularDamping;b.friction=a.friction;b.restitution=a.restitution;mmv::Identity(b.world);
        if(auto* body=static_cast<btRigidBody*>(a.body)){
            b.valid=1;
            if(snapshotNativePhysics){
                Matrix(body->getWorldTransform(),b.world);
                for(int c=0;c<3;++c){b.linearVelocity[c]=float(body->getLinearVelocity()[c]);b.angularVelocity[c]=float(body->getAngularVelocity()[c]);}
            }else{
                // Native bodies stop updating when MMV takes over physics.
                // Export the animation pose, not their last solver state.
                float local[16];RigidBindingMatrix(a,local);
                const bool linked=a.boneIndex>=0&&uint32_t(a.boneIndex)<s->bones.size();
                if(linked&&!s->bones[a.boneIndex].valid)b.valid=0;
                else MultiplyPhysicsMatrix(b.world,local,linked?s->bones[a.boneIndex].world:o.world);
            }
        }
    }
    if(demand&MMV_INPUT_DEMAND_JOINTS)s->joints.resize(m.jointCount);
    if(demand&MMV_INPUT_DEMAND_JOINTS)for(uint32_t i=0;i<m.jointCount;++i){const auto& a=m.jointTable[i];auto& b=s->joints[i];b.name=s->Keep(Name(a.jpText,a.name));b.rigidA=a.rigidA;b.rigidB=a.rigidB;mmv::Identity(b.anchorA);mmv::Identity(b.anchorB);
        memcpy(b.linearLower,a.limits+3,12);memcpy(b.linearUpper,a.limits,12);memcpy(b.angularLower,a.limits+9,12);memcpy(b.angularUpper,a.limits+6,12);memcpy(b.springs,a.springs,24);
        if(a.constraint>=0&&m.scenePtr&&m.scenePtr->world)for(int j=0;j<m.scenePtr->world->getNumConstraints();++j){auto* c=m.scenePtr->world->getConstraint(j);if(c->getUserConstraintId()!=a.constraint)continue;
            if(auto* joint=dynamic_cast<btGeneric6DofConstraint*>(c)){
                if(snapshotNativePhysics){Matrix(joint->getRigidBodyA().getWorldTransform()*joint->getFrameOffsetA(),b.anchorA);Matrix(joint->getRigidBodyB().getWorldTransform()*joint->getFrameOffsetB(),b.anchorB);b.valid=1;}
                else if(a.rigidA>=0&&a.rigidB>=0&&uint32_t(a.rigidA)<s->rigids.size()&&uint32_t(a.rigidB)<s->rigids.size()&&s->rigids[a.rigidA].valid&&s->rigids[a.rigidB].valid){
                    float local[16];Matrix(joint->getFrameOffsetA(),local);MultiplyPhysicsMatrix(b.anchorA,local,s->rigids[a.rigidA].world);
                    Matrix(joint->getFrameOffsetB(),local);MultiplyPhysicsMatrix(b.anchorB,local,s->rigids[a.rigidB].world);b.valid=1;
                }
            }break;}
    }
    return s;
}
std::unique_ptr<Snapshot> AccessorySnapshot(mdl::AccessoryRecord* a,uint32_t instance,uint32_t demand) {
    auto s=std::make_unique<Snapshot>();auto& o=s->object;o.structureSize=sizeof(o);o.kind=MMV_ACCESSORY;o.instance=instance;o.visible=a->visible;
    if(!a->shadowEnabled||a->additiveBlend)s->renderData.flags|=MMV_OBJECT_RENDER_NO_SELF_SHADOW;
    o.id=reinterpret_cast<uintptr_t>(a);o.generation=Generation(a);s->path=a->sourcePath;o.name=s->Keep(Utf8(std::filesystem::path(s->path).filename().c_str()));
    // Use the same public accessory transform query as MME, including parents.
    mmv::Identity(o.world);
    int ordinal=0;for(int i=0;i<255;++i){if(!host->AccessorySlot(i))continue;if(host->AccessorySlot(i)==a){ExpGetAcsWorldMat(o.world,ordinal);break;}++ordinal;}
    const bool needVertices=(demand&MMV_INPUT_DEMAND_VERTICES)!=0;
    const bool needTriangles=(demand&MMV_INPUT_DEMAND_TRIANGLES)!=0;
    const bool needMaterials=(demand&MMV_INPUT_DEMAND_MATERIALS)!=0;
    const bool needOriginal=(demand&MMV_INPUT_DEMAND_ORIGINAL_VERTICES)!=0;
    const bool needWeights=(demand&MMV_INPUT_DEMAND_WEIGHTS)!=0;
    if(!needVertices&&!needTriangles){
        if(needMaterials){
            s->mesh.materials.resize(a->materialCount);
            for(uint32_t i=0;i<a->materialCount;++i){auto& b=s->mesh.materials[i];const auto& m=static_cast<const D3DMATERIAL9*>(a->materials)[i];b.key="source:"+std::to_string(i);b.name="subset"+std::to_string(i);b.value.sourceMaterial=i;
                memcpy(b.value.diffuse,&m.Diffuse,16);memcpy(b.value.ambient,&m.Ambient,12);memcpy(b.value.specular,&m.Specular,12);b.value.power=m.Power;
                if(a->texturePaths){const auto* p=reinterpret_cast<const wchar_t*>(a->texturePaths[i]);b.texture.path=p;b.sphere.path=p+512;}
            }
        }
        return s;
    }
    // The loader already normalizes native accessories to this layout. Read
    // their live buffers directly; cloning every frame copies the entire mesh
    // through D3DX and allocates temporary GPU buffers for an unchanged layout.
    // Preserve conversion for nonstandard producers, and copy into our owned
    // CPU snapshot so later host edits cannot change the published frame.
    constexpr DWORD fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1;
    void* clone=a->mesh;ComPtr<IUnknown> owner;
    if(d3dx::MeshFvf(a->mesh)==fvf)owner=static_cast<IUnknown*>(a->mesh);
    else {
        clone=nullptr;Check(d3dx::CloneMeshFvf(a->mesh,0x220|0x1,fvf,host->Renderer()->device,&clone),"Clone accessory snapshot");
        owner.Attach(static_cast<IUnknown*>(clone));
    }
    const auto vertices=d3dx::MeshMethod<DWORD(WINAPI*)(void*),5>(clone)(clone);
    const auto faces=d3dx::MeshMethod<DWORD(WINAPI*)(void*),4>(clone)(clone);
    ComPtr<IDirect3DVertexBuffer9> vb;ComPtr<IDirect3DIndexBuffer9> ib;Check(d3dx::MeshVertexBuffer(clone,&vb),"Accessory VB");Check(d3dx::MeshIndexBuffer(clone,&ib),"Accessory IB");
    if(needVertices||needOriginal||needWeights)ReadVertices(vb.Get(),vertices,32,s->mesh.vertices);
    if(needTriangles)ReadTriangles(ib.Get(),faces*3,s->mesh.triangles);
    if(needOriginal){s->original=s->mesh.vertices;s->originalCount=vertices;}
    if(needWeights){s->weights.resize(vertices);s->originalCount=vertices;}
    // Accessory meshes are not skinned.  resize() zero-initializes each weight
    // row, which would incorrectly name bone 0 even though the accessory has
    // no bone table; MMV validates every non-negative index, including slots
    // whose weight is zero.
    if(needWeights)for(auto& weight:s->weights)for(int slot=0;slot<4;++slot)weight.bones[slot]=-1;
    if(needTriangles&&needMaterials){DWORD* attributes=nullptr;Check(d3dx::MeshMethod<HRESULT(WINAPI*)(void*,DWORD,DWORD**),24>(clone)(clone,D3DLOCK_READONLY,&attributes),"Accessory attributes");
        for(uint32_t i=0;i<faces;++i)s->mesh.triangles[i].material=attributes[i];d3dx::MeshMethod<HRESULT(WINAPI*)(void*),25>(clone)(clone);}
    if(needMaterials){s->mesh.materials.resize(a->materialCount);
        for(uint32_t i=0;i<a->materialCount;++i){auto& b=s->mesh.materials[i];const auto& m=static_cast<const D3DMATERIAL9*>(a->materials)[i];b.key="source:"+std::to_string(i);b.name="subset"+std::to_string(i);b.value.sourceMaterial=i;
            memcpy(b.value.diffuse,&m.Diffuse,16);memcpy(b.value.ambient,&m.Ambient,12);memcpy(b.value.specular,&m.Specular,12);b.value.power=m.Power;
            if(a->texturePaths){const auto* p=reinterpret_cast<const wchar_t*>(a->texturePaths[i]);b.texture.path=p;b.sphere.path=p+512;}
        }
    }
    return s;
}

#include "lifecycle.inl"
}
#include "menu.inl"
#include "renderer.inl"
}
