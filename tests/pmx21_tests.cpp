// MAINTENANCE: keep until PMX 2.1 import, deformation and physics have equivalent coverage.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/pmx21.hpp"
#include "mikumikustudio/pmx21_morph.hpp"
#include "mikumikustudio/qdef_skinning.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/path_workspace.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/history_physics.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/d3d12_skinning.hpp"
#include "mikumikustudio/soft_body_compute.hpp"
#include "mikumikustudio/scene_sampling.hpp"
#include "../src/plugins/scene_resource_budget.hpp"
#include "../src/physics/soft_body_topology.hpp"
#include "btBulletDynamicsCommon.h"
#include "BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h"
#include "LinearMath/btQuickprof.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <io.h>
#include <memory>
#include <unordered_map>
#include <vector>
using namespace mikumikustudio;
namespace {
using Bytes = std::vector<unsigned char>;
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
bool Near(float a, float b) { return std::fabs(a-b) < 2e-4f; }
template<class T> void Put(Bytes& b, T v) { const auto* p = reinterpret_cast<const unsigned char*>(&v); b.insert(b.end(), p, p+sizeof v); }
void Floats(Bytes& b, std::initializer_list<float> v) { for (float x : v) Put(b,x); }
void Index(Bytes& b, int v, int width) { for (int i = 0; i < width; ++i) Put(b,static_cast<unsigned char>(unsigned(v)>>(i*8))); }
void Text(Bytes& b, bool utf8, const wchar_t* text = L"测试_A_\U0001f31f") {
    const unsigned n = unsigned(std::wcslen(text));
    if (!utf8) {
        Put(b,n*2); const auto* p = reinterpret_cast<const unsigned char*>(text); b.insert(b.end(),p,p+n*2);
    } else {
        const int length = WideCharToMultiByte(CP_UTF8,0,text,int(n),nullptr,0,nullptr,nullptr);
        Put(b,unsigned(length)); const auto first = b.size(); b.resize(first+length);
        if (length) WideCharToMultiByte(CP_UTF8,0,text,int(n),reinterpret_cast<char*>(b.data()+first),length,nullptr,nullptr);
    }
}
Bytes Fixture(int width, bool utf8, bool soft = true, unsigned char shape = 0, int anchor = -1) {
    Bytes b{'P','M','X',' '}; Put(b,2.1f);
    for (unsigned char v : std::array<unsigned char,9>{8,static_cast<unsigned char>(utf8),0,static_cast<unsigned char>(width),static_cast<unsigned char>(width),static_cast<unsigned char>(width),static_cast<unsigned char>(width),static_cast<unsigned char>(width),static_cast<unsigned char>(width)}) Put(b,v);
    for (int i = 0; i < 4; ++i) Text(b,utf8);
    Put(b,3u);
    for (int i = 0; i < 3; ++i) {
        Floats(b,{float(i==1),5.f+float(i==2),0,0,0,1,0,0}); Put(b,static_cast<unsigned char>(4));
        for (int bone : {0,1,-1,-1}) Index(b,bone,width);
        Floats(b,{.5f,.5f,0,0,1});
    }
    Put(b,3u); for (int i=0;i<3;++i) Index(b,i,width);
    Put(b,0u); Put(b,1u); Text(b,utf8); Text(b,utf8);
    Floats(b,{.7f,.6f,.5f,1,0,0,0,1,.2f,.2f,.2f}); Put(b,static_cast<unsigned char>(31));
    Floats(b,{0,0,0,1,1}); Index(b,-1,width); Index(b,-1,width);
    Put(b,static_cast<unsigned char>(0)); Put(b,static_cast<unsigned char>(1)); Put(b,static_cast<unsigned char>(0)); Text(b,utf8,L""); Put(b,3u);
    Put(b,2u);
    for (int i=0;i<2;++i) {
        Text(b,utf8); Text(b,utf8); Floats(b,{0,0,0}); Index(b,-1,width); Put(b,0u); Put(b,static_cast<unsigned short>(0)); Floats(b,{0,0,0});
    }
    Put(b,4u);
    const auto morph = [&](unsigned char type,int count) { Text(b,utf8);Text(b,utf8);Put(b,static_cast<unsigned char>(1));Put(b,type);Put(b,count); };
    morph(2,1); Index(b,0,width); Floats(b,{2,0,0,0,0,0,1});
    morph(9,2); Index(b,0,width); Put(b,.5f); Index(b,0,width); Put(b,1.f);
    morph(10,1); Index(b,0,width); Put(b,static_cast<unsigned char>(1)); Floats(b,{2,0,0,0,1,0});
    morph(0,1); Index(b,1,width); Put(b,.5f);
    Put(b,0u); Put(b,2u);
    for (int i=0;i<2;++i) {
        Text(b,utf8);Text(b,utf8);Index(b,0,width);Put(b,static_cast<unsigned char>(0));Put(b,static_cast<unsigned short>(0));Put(b,static_cast<unsigned char>(0));
        Floats(b,{.1f,.1f,.1f,float(i),5,0,0,0,0,2,0,0,0,.5f});Put(b,static_cast<unsigned char>(1));
    }
    Put(b,6u);
    for (unsigned char type=0;type<6;++type) {
        Text(b,utf8);Text(b,utf8);Put(b,type);Index(b,0,width);Index(b,1,width);
        Floats(b,{0,5,0,0,0,0,-.1f,-.1f,-.1f,.1f,.1f,.1f,-.1f,-.1f,-.1f,.1f,.1f,.1f});
        Floats(b,{.9f,.3f,1,0,0,0});
    }
    Put(b,soft?1u:0u);
    if (soft) {
        Text(b,utf8);Text(b,utf8);Put(b,shape);Index(b,0,width);
        Put(b,static_cast<unsigned char>(1));Put(b,static_cast<unsigned short>(0));Put(b,static_cast<unsigned char>(0));
        Put(b,0);Put(b,0);Put(b,1.f);Put(b,.01f);Put(b,0);
        Floats(b,{1,.01f,0,0,0,0,.2f,0,1,.1f,1,.7f,.1f,1,.5f,.5f,.5f,.5f});
        for (int i : {0,4,0,4}) Put(b,i);
        Floats(b,{1,1,1}); Put(b,anchor<0?0u:1u);
        if(anchor>=0){Index(b,0,width);Index(b,1,width);Put(b,static_cast<unsigned char>(anchor));}
        Put(b,1u); Index(b,0,width);
    }
    return b;
}
struct Host {
    HWND window = CreateWindowExW(0,L"STATIC",L"PMX 2.1 test",0,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    btSoftBodyRigidBodyCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    history_physics::Broadphase broadphase{btVector3(-10000,-10000,-10000),btVector3(10000,10000,10000),1024};
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher,&broadphase,&solver,&config}; PhysicsScene scene{};
    Host() {
        renderer->hwnd=window; renderer->d3d9=Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=window;p.BackBufferWidth=p.BackBufferHeight=64;
        Check(renderer->d3d9 && SUCCEEDED(renderer->d3d9->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,&renderer->device)),"real hidden D3D9 device");
        scene.world=&world;scene.dispatcher=&dispatcher;scene.broadphase=&broadphase;
        world.setGravity(btVector3(0,-9.8f,0));world.ResetLocalTime();
    }
    ~Host() { if(renderer->device)renderer->device->Release();if(renderer->d3d9)renderer->d3d9->Release();DestroyWindow(window); }
};
struct Model {
    std::unique_ptr<mdl::ModelRecord> data = std::make_unique<mdl::ModelRecord>();
    unsigned char* raw() { return reinterpret_cast<unsigned char*>(data.get()); }
    ~Model() { ModelDispose(raw()); }
};
std::size_t UniqueSdfCells(const btSparseSdf<3>& sdf) {
    std::vector<std::array<std::intptr_t,4>> cells;
    for(int i=0;i<sdf.cells.size();++i)for(auto* c=sdf.cells[i];c;c=c->next)
        cells.push_back({c->c[0],c->c[1],c->c[2],reinterpret_cast<std::intptr_t>(c->pclient)});
    std::sort(cells.begin(),cells.end());
    return std::size_t(std::unique(cells.begin(),cells.end())-cells.begin());
}
void SdfCache(Host& host) {
    auto& info=host.world.getWorldInfo();
    info.m_sparsesdf.Reset();
    btSoftBody body(&info,0,nullptr,nullptr);
    btSphereShape sphere(.6f);btBoxShape box(btVector3(.6f,1,.8f));btCapsuleShape capsule(.5f,1);
    btCollisionObject object;object.setWorldTransform(btTransform::getIdentity());
    for(auto* shape:std::array<btCollisionShape*,3>{&sphere,&box,&capsule}) {
        object.setCollisionShape(shape);
        for(int repeat=0;repeat<8;++repeat)for(int i=0;i<64;++i) {
            const btVector3 position(float(i%4)*.3f-.6f,float((i/4)%4)*.3f-.6f,float(i/16)*.3f-.6f);
            btSoftBody::sCti contact;
            body.checkContact(&object,position,.01f,contact);
        }
    }
    Check(UniqueSdfCells(info.m_sparsesdf)==std::size_t(info.m_sparsesdf.ncells),"x64 SDF cache has one entry per shape and cell");
    info.m_sparsesdf.Reset();
}
void Topology() {
    btSoftBodyWorldInfo world;
    std::vector<btVector3> positions;
    for(int i=0;i<49;++i) positions.emplace_back(float(i%7),float(i/7),0);
    std::vector<btScalar> masses(positions.size(),1);
    for(int kind=0;kind<4;++kind)for(int distance:{0,1,2,3,4,6,49}) {
        btSoftBody reference(&world,int(positions.size()),positions.data(),masses.data());
        btSoftBody sparse(&world,int(positions.size()),positions.data(),masses.data());
        std::vector<int> faces;
        for(int y=0;y<6;++y)for(int x=0;x<6;++x) {
            if(kind==1 && x==3)continue;
            const int a=y*7+x;
            faces.insert(faces.end(),{a,a+1,a+7,a+1,a+8,a+7});
        }
        faces.insert(faces.end(),{0,1,7,7,1,0,0,0,1});
        if(kind<2) {
            for(std::size_t i=0;i<faces.size();i+=3) {
                const int a=faces[i],b=faces[i+1],c=faces[i+2];
                if(a==b || b==c || a==c)continue;
                reference.appendFace(a,b,c);
                reference.appendLink(a,b,nullptr,true);reference.appendLink(b,c,nullptr,true);reference.appendLink(c,a,nullptr,true);
            }
            pmx21::topology::AppendFaces(sparse,faces,sparse.m_materials[0]);
        } else {
            unsigned random=761;
            for(int i=0;i<150;++i) {
                random=random*1664525u+1013904223u;const int a=int(random%49);
                random=random*1664525u+1013904223u;const int b=kind==2 ? (a+1)%49 : int(random%49);
                if(a!=b){reference.appendLink(a,b,nullptr,true);sparse.appendLink(a,b,nullptr,true);}
            }
        }
        const int expected=reference.generateBendingConstraints(distance,reference.m_materials[0]);
        const int actual=pmx21::topology::GenerateBending(sparse,distance,sparse.m_materials[0]);
        Check(expected==actual && reference.m_links.size()==sparse.m_links.size(),"sparse bending count matches Bullet");
        for(int i=0;i<std::min(reference.m_links.size(),sparse.m_links.size());++i) {
            const auto& a=reference.m_links[i];const auto& b=sparse.m_links[i];
            Check(a.m_n[0]-&reference.m_nodes[0]==b.m_n[0]-&sparse.m_nodes[0] &&
                  a.m_n[1]-&reference.m_nodes[0]==b.m_n[1]-&sparse.m_nodes[0] &&
                  a.m_bbending==b.m_bbending && a.m_rl==b.m_rl,
                  "sparse topology retains endpoints, order, bending flags and rest lengths");
        }
    }
    std::printf("soft_topology_failures=%d\n",failures);
}
bool Load(Host& host, Model& model, const Bytes& bytes, PhysicsScene* scene = nullptr) {
    wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
    if(!GetCurrentDirectoryW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"p21",0,path)){Check(false,"create fixture file");return false;}
    int fd=_wopen(path,_O_BINARY|_O_WRONLY|_O_TRUNC);
    if(fd<0){Check(false,"open fixture output");DeleteFileW(path);return false;}
    Check(_write(fd,bytes.data(),unsigned(bytes.size()))==int(bytes.size()),"write complete fixture");_close(fd);
    fd=_wopen(path,_O_BINARY|_O_RDONLY);
    if(fd<0){Check(false,"open fixture input");DeleteFileW(path);return false;}
    _lseek(fd,3,SEEK_SET);
    model.data->scenePtr=scene ? scene : &host.scene;PathResolutionWorkspace paths{};
    const bool result=LoadPMX(model.raw(),host.renderer.get(),0,1,paths,fd);
    Check(DeleteFileW(path)!=FALSE,"fixture closed after load");return result;
}
void Qdef() {
    float matrices[2][16]{}; affine::BoneAux aux[2];
    for(int i=0;i<2;++i) { for(int c=0;c<4;++c)matrices[i][c*5]=1; affine::PrecomputeBone(matrices[i],nullptr,aux[i]); }
    const float p[3]{1,0,0},n[3]{1,0,0},weights[4]{.5f,.5f,0,0}; const int bones[4]{0,1,-1,-1};float out[3],normal[3];
    auto skin=[&]{affine::SkinQdef(p,n,bones,weights,2,[&](int b){return matrices[b];},aux,out,normal);};
    matrices[1][12]=4;skin();Check(Near(out[0],3)&&Near(out[1],0),"QDEF blends translation");
    const float quarter[4]{0,0,std::sqrt(.5f),std::sqrt(.5f)};
    affine::DefaultMatrixFromQuaternion(matrices[1],quarter);affine::PrecomputeBone(matrices[1],nullptr,aux[1]);
    skin();Check(Near(out[0],std::sqrt(.5f))&&Near(out[1],std::sqrt(.5f)),"QDEF uses the native rotation direction");
    const float q[4]{0,0,1,0};affine::DefaultMatrixFromQuaternion(matrices[1],q);affine::PrecomputeBone(matrices[1],nullptr,aux[1]);
    skin();Check(Near(out[0],0)&&Near(std::fabs(out[1]),1),"QDEF half turn preserves radius unlike BDEF4");
    matrices[0][0]=2;affine::PrecomputeBone(matrices[0],nullptr,aux[0]);skin();
    Check(Near(std::sqrt(out[0]*out[0]+out[1]*out[1]),1.5f),"QDEF incorporates bone scale");
    Check(Near(std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]),1),"QDEF normal normalized");
}
void VertexColorShader(Host& host) {
    std::ifstream input(MMS_SOURCE_DIR "/res/assets/skin_effect_sm2.fx",std::ios::binary);
    std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    auto& api=d3dx::Get();Check(api.Load(),"load native D3DX");
    auto* device=host.renderer->device;void* effect=nullptr;void* errors=nullptr;
    const HRESULT compiled=api.createEffect(device,source.data(),UINT(source.size()),nullptr,nullptr,0,nullptr,&effect,&errors);
    if(errors)static_cast<IUnknown*>(errors)->Release();
    Check(SUCCEEDED(compiled)&&effect,"compile SM2 PMX material effect");if(!effect)return;
    IDirect3DSurface9 *target=nullptr,*readback=nullptr,*previous=nullptr;
    device->GetRenderTarget(0,&previous);
    Check(SUCCEEDED(device->CreateRenderTarget(64,64,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr)),"create color target");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr)),"create color readback");
    if(target&&readback) {
        device->SetRenderTarget(0,target);device->SetDepthStencilSurface(nullptr);
        d3dx::D3DXMATRIXF identity{};for(int c=0;c<4;++c)reinterpret_cast<float*>(&identity)[c*5]=1;
        const float white[4]{1,1,1,1},zero[4]{},light[4]{0,0,1,0};
        d3dx::FxSetMatrix(effect,"matWorldViewProj",&identity);d3dx::FxSetMatrix(effect,"matLightViewProj",&identity);
        d3dx::FxSetFloatArray(effect,"EgColor",white,4);d3dx::FxSetFloatArray(effect,"ToonColor",white,4);
        d3dx::FxSetFloatArray(effect,"SpcColor",zero,4);d3dx::FxSetFloatArray(effect,"LightDir",light,4);
        d3dx::FxSetInt(effect,"MdsPmxUnshadowed",1);d3dx::FxSetTechnique(effect,"BufferShadowTec");
        const float positions[3][3]{{-.8f,-.8f,.5f},{0,.8f,.5f},{.8f,-.8f,.5f}};
        for(bool color:{false,true}) {
            std::vector<float> vertices;
            for(auto& p:positions) {
                vertices.insert(vertices.end(),p,p+3);vertices.insert(vertices.end(),{0,0,-1,0,0});
                if(color)vertices.insert(vertices.end(),{0,1,0,.5f});
            }
            device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0);Check(SUCCEEDED(device->BeginScene()),"begin shader draw");
            d3dx::FxSetInt(effect,"MdsPmxVertexColor",color);UINT passes=0;
            Check(SUCCEEDED(d3dx::FxBegin(effect,&passes))&&SUCCEEDED(d3dx::FxBeginPass(effect,0)),"begin material effect");
            device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_ZENABLE,FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
            device->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|(color ? D3DFVF_TEX2|D3DFVF_TEXCOORDSIZE4(1) : D3DFVF_TEX1));
            Check(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices.data(),color?48:32)),"draw PMX and legacy UV layouts");
            d3dx::FxEndPass(effect);d3dx::FxEnd(effect);device->EndScene();
            Check(SUCCEEDED(device->GetRenderTargetData(target,readback)),"read material pixels");
            D3DLOCKED_RECT locked{};
            if(SUCCEEDED(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY))) {
                const auto pixel=*reinterpret_cast<const unsigned*>(static_cast<const char*>(locked.pBits)+32*locked.Pitch+32*4);
                Check(color ? (pixel&0xFFFFFF)==0x00FF00&&((pixel>>24)==127||(pixel>>24)==128) : pixel==0xFFFFFFFF,
                    color?"UV1 RGBA modulates native material pixel":"legacy vertex layout remains white");
                readback->UnlockRect();
            } else Check(false,"lock pixel readback");
        }
    }
    device->SetRenderTarget(0,previous);if(previous)previous->Release();
    if(readback)readback->Release();if(target)target->Release();static_cast<IUnknown*>(effect)->Release();
}
void Integration(Host& host,int width,bool utf8) {
    const auto texts=owned_text::Count();
    {
        Model model;Check(Load(host,model,Fixture(width,utf8)),"load full PMX 2.1 fixture");
        if(!model.data->loadComplete)return;
        auto& m=*model.data;
        Check(m.vertexCount==3&&m.pmxVertices[2].weightType==mdl::PmxWeightType::qdef&&m.pmxVertices[2].bone[1]==1&&Near(m.pmxVertices[2].weight[1],.5f),"QDEF preserves stream alignment and all influences");
        Check(m.morphCount==4&&m.morphs[1].type==9&&m.morphs[2].type==10&&m.rigidCount==2&&m.jointCount==6,"new morph records preserve following tables");
        Check(std::wcscmp(m.boneTable[0].jpText,L"测试_A_\U0001f31f")==0,"UTF16 and UTF8 names preserve Unicode");
        const int types[]{D6_CONSTRAINT_TYPE,D6_CONSTRAINT_TYPE,POINT2POINT_CONSTRAINT_TYPE,CONETWIST_CONSTRAINT_TYPE,SLIDER_CONSTRAINT_TYPE,HINGE_CONSTRAINT_TYPE};
        Check(host.world.getNumConstraints()==6,"all six PMX joint types created");
        for(int i=0;i<host.world.getNumConstraints();++i)Check(host.world.getConstraint(i)->getConstraintType()==types[i],"native Bullet constraint matches PMX kind");
        const auto measure=[&] {
            plugins::resource_budget::Collector budget;plugins::resource_budget::Model(m,budget);
            for(int i=0;i<host.world.getNumConstraints();++i)plugins::resource_budget::Joint(host.world.getConstraint(i),budget);
            if(!budget.Result().Complete())std::printf("PMX budget missing=0x%x\n",budget.Result().missing);
            Check(budget.Result().Complete(),"PMX 2.1 resources admit native import/undo budgeting");
            return budget.Result().totalBytes;
        };
        const auto definitionBytes=measure();
        m.morphs[1].value=.5f;ModelApplyMorphs(model.raw());Check(Near(mdl::BoneMorphOffsets(model.raw())[0].translation[0],1),"flip selects a fixed target weight");
        Check(m.morphs[0].value==0,"flip leaves authored slider unchanged");
        m.morphs[1].value=.1f;ModelApplyMorphs(model.raw());Check(Near(mdl::BoneMorphOffsets(model.raw())[0].translation[0],0),"flip zero interval clears previous result");
        m.morphs[3].value=1;ModelApplyMorphs(model.raw());Check(Near(mdl::BoneMorphOffsets(model.raw())[0].translation[0],1),"group drives flip selection");m.morphs[3].value=0;
        auto* rigid=static_cast<btRigidBody*>(m.rigidTable[0].body);rigid->setLinearVelocity(btVector3(0,0,0));m.morphs[2].value=1;
        ModelApplyMorphs(model.raw());Check(Near(rigid->getLinearVelocity().length(),0),"visual morph evaluation applies no impulse");
        pmx21::ApplyImpulses(model.raw());Check(Near(rigid->getLinearVelocity().x(),1),"impulse uses body mass");m.morphs[2].value=0;
        for(unsigned i=0;i<m.boneCount;++i){std::memset(m.boneTable[i].matInit,0,64);for(int c=0;c<4;++c)m.boneTable[i].matInit[c*5]=1;}
        pmx21::SyncSoftBodies(model.raw(),true);Check(host.world.getSoftBodyArray().size()==1,"soft mesh registered in native world");
        if(host.world.getSoftBodyArray().size()) {
            auto* soft=host.world.getSoftBodyArray()[0];Check(soft->m_nodes.size()==3&&soft->m_nodes[0].m_im==0,"soft mesh pin mapped to original vertex");
            const float initial=soft->m_nodes[1].m_x.y();for(int i=0;i<6;++i)host.world.stepSimulation(1.f/60,1,1.f/60);
            Check(measure()>definitionBytes,"budget includes live soft body allocations");
            Check(soft->m_nodes[1].m_x.y()<initial,"soft mesh responds to gravity");Check(Near(soft->m_nodes[0].m_x.y(),5),"pinned soft vertex stays fixed");
            mdl::SkinnedVertexBase output[3]{};mdl::EdgeVertex edges[3]{};pmx21::WriteSoftVertices(model.raw(),output,sizeof output[0],edges,.1f);
            Check(Near(output[1].position[1],soft->m_nodes[1].m_x.y()),"simulation reaches displayed vertex buffer");
            pmx21::SyncSoftBodies(model.raw(),true);Check(Near(soft->m_nodes[1].m_x.y(),initial),"seek reset restores soft pose");
            {
                auto app=std::make_unique<MMDApp>();app->state=*std::make_unique<MMDAppState>();
                app->Physics()=&host.scene;app->ModelSlot(0)=model.raw();
                app->GravityY()=-1;app->GravityMagnitude()=.98f;
                bake::Request request;request.lastFrame=1;request.keyframes=true;
                const auto position=soft->m_nodes[1].m_x;const auto velocity=rigid->getLinearVelocity();
                const auto baked=bake::ComputeScene(*app,request);
                Check(baked.error==bake::Error::None&&!baked.models.empty(),"isolated bake handles soft body and all joint types");
                Check(soft->m_nodes[1].m_x==position&&rigid->getLinearVelocity()==velocity,"bake preserves live physics state");
                Check(host.world.getSoftBodyArray().size()==1&&host.world.getNumConstraints()==6,"bake releases only private physics");
                if (width == 2 && utf8) {
                    app->PlaybackPhysicsMode() = 3;
                    app->state.messageSeen = 1;
                    app->state.deltaTime = 0.5f;
                    app->GravityMagnitude() = .98f;
                    soft->m_nodes[1].m_v.setValue(0, -1, 0);
                    const auto pausedPosition = soft->m_nodes[1].m_x;
                    const auto pausedVelocity = soft->m_nodes[1].m_v;
                    const auto serial = host.world.StepSerial();
                    for (int idle = 0; idle < 3; ++idle) PhysicsFrame(app.get(), 1);
                    Check(host.world.StepSerial() == serial &&
                        soft->m_nodes[1].m_x == pausedPosition &&
                        soft->m_nodes[1].m_v == pausedVelocity,
                        "trace pause keeps soft state and ignores selection and idle time");
                    app->state.physicsBodiesMoved = 1;
                    PhysicsFrame(app.get(), 0);
                    Check(host.world.StepSerial() > serial &&
                        soft->m_nodes[1].m_x != pausedPosition,
                        "explicit trace frame advance steps the soft-body world");
                    const auto livePosition = soft->m_nodes[1].m_x;
                    const auto liveVelocity = soft->m_nodes[1].m_v;
                    const auto liveSerial = host.world.StepSerial();
                    mdl::CameraKey camera{};
                    mdl::LightKey light{};
                    mdl::SelfShadowKey shadow{};
                    camera.fov = 30; camera.parentModel = camera.parentBone = -1;
                    app->CameraKeys() = &camera;
                    app->LightKeys() = &light;
                    app->ShadowKeys() = &shadow;
                    app->Renderer() = host.renderer.get();
                    std::vector<float> samples;
                    std::string error;
                    const bool sampled = scene_sampling::Visit(*app,0,2,30,
                        [&](double, bool outputSample) {
                            if (outputSample) {
                                mdl::SkinnedVertexBase vertices[3]{};
                                mdl::EdgeVertex edges[3]{};
                                pmx21::WriteSoftVertices(model.raw(), vertices, sizeof vertices[0], edges, 0.f);
                                samples.push_back(vertices[1].position[1]);
                            }
                            return true;
                        }, error);
                    if (!sampled) std::printf("soft mesh sample: %s\n", error.c_str());
                    // Mesh sampling runs the same startup settling steps as playback.
                    Check(sampled && samples.size() == 3 && samples.front() <= initial &&
                        samples.front() > initial - .1f &&
                        samples.back() < samples.front(),
                        "mesh sampler renders private soft deformation at each output frame");
                    Check(host.world.StepSerial() == liveSerial && soft->m_nodes[1].m_x == livePosition &&
                        soft->m_nodes[1].m_v == liveVelocity,
                        "mesh sampling restores the editor soft-body identity and state");
                    app->CameraKeys() = nullptr;
                    app->LightKeys() = nullptr;
                    app->ShadowKeys() = nullptr;
                    app->Renderer() = nullptr;
                }
                app->ModelSlot(0)=nullptr;app->Physics()=nullptr;
            }
            {
                history_physics::ImportScene staging(host.scene);Model peer;
                Check(Load(host,peer,Fixture(width,utf8,false),&staging.Scene()),"parse another model in private world");
                auto publication=staging.Prepare();Check(publication->IsCurrent()&&publication->Publish(),"import publishes while soft peer is live");
                peer.data->scenePtr=&host.scene;publication.reset();
                Check(host.world.getSoftBodyArray().size()==1,"import preserves existing soft body");
            }
            std::vector<btRigidBody*> rigids;std::vector<btTypedConstraint*> joints;
            for(unsigned i=0;i<m.rigidCount;++i)rigids.push_back(static_cast<btRigidBody*>(m.rigidTable[i].body));
            for(int i=0;i<host.world.getNumConstraints();++i)joints.push_back(host.world.getConstraint(i));
            history_physics::Retained retained(host.world,rigids,joints);
            { auto remove=retained.Prepare(false);Check(remove->Publish(),"park rigid history with soft peer");pmx21::SetPresent(model.raw(),false); }
            Check(host.world.getSoftBodyArray().size()==0,"model deletion removes soft anchors");
            { auto restore=retained.Prepare(true);Check(restore->Publish(),"restore rigid history after soft removal");pmx21::SetPresent(model.raw(),true); }
            pmx21::SyncSoftBodies(model.raw(),true);Check(host.world.getSoftBodyArray().size()==1,"undo recreates soft mesh");
        }
    }
    Check(host.world.getNumConstraints()==0&&host.world.getNumCollisionObjects()==0&&host.world.getSoftBodyArray().size()==0,"dispose removes only model physics including soft mesh");
    Check(owned_text::Count()==texts,"PMX 2.1 text ownership balanced");
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (argc >= 3 && !std::wcscmp(argv[1], L"--model")) {
        Host host; Model model;
        if (!host.renderer->device) return 1;
        const std::wstring path = argv[2];
        const auto slash = path.find_last_of(L"/\\");
        if (slash != std::wstring::npos)
            wcscpy_s(model.data->modelDirectory, path.substr(0, slash + 1).c_str());
        model.data->scenePtr = &host.scene;
        int fd = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
        if (fd < 0) return 2;
        _lseek(fd, 3, SEEK_SET);
        PathResolutionWorkspace paths{};
        const auto elapsed = [](auto start) {
            return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        };
        std::puts("model phase=read begin");
        auto start = std::chrono::steady_clock::now();
        if (!LoadPMX(model.raw(), host.renderer.get(), 0, 1, paths, fd)) return 3;
        std::printf("model phase=read ms=%.3f vertices=%u\n", elapsed(start), model.data->vertexCount);
        for (unsigned i=0; i<model.data->boneCount; ++i) {
            auto* matrix=model.data->boneTable[i].matInit;
            std::memset(matrix,0,64); for (int c=0;c<4;++c) matrix[c*5]=1;
        }
        std::puts("model phase=soft_init begin");
        start = std::chrono::steady_clock::now();
        pmx21::SyncSoftBodies(model.raw(),true);
        std::printf("model phase=soft_init ms=%.3f bodies=%d\n", elapsed(start), host.world.getSoftBodyArray().size());
        for (int i=0;i<host.world.getSoftBodyArray().size();++i) {
            const auto& body=*host.world.getSoftBodyArray()[i];
            std::printf("body=%d nodes=%d links=%d faces=%d clusters=%d\n",i,body.m_nodes.size(),body.m_links.size(),body.m_faces.size(),body.m_clusters.size());
        }
        static std::unordered_map<btSoftBody*,soft_compute::State> states;
        static processor::Selection choice{processor::Mode::Cpu};
        if (argc >= 4 && !std::wcscmp(argv[3],L"--gpu")) {
            const auto adapters=gpu::EnumerateAdapters();
            if (adapters.empty()) return 4;
            choice={processor::Mode::Adapter,adapters[0].id,adapters[0].name};
        } else if(argc >= 4 && !std::wcscmp(argv[3],L"--auto")) choice={processor::Mode::Automatic};
        DynamicsWorld::solveSoftBody=[](btSoftBody& body) {
            const auto start=std::chrono::steady_clock::now();
            if (!states[&body].Solve(body,choice)) body.solveConstraints();
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::printf("model solve nodes=%d contacts=%d ms=%.3f\n",body.m_nodes.size(),body.m_scontacts.size()+body.m_rcontacts.size(),ms);
            return true;
        };
        static double collisionMs[3]{};
        static unsigned collisionCalls[3]{};
        host.dispatcher.setNearCallback([](btBroadphasePair& pair,btCollisionDispatcher& dispatcher,const btDispatcherInfo& info) {
            const auto* a=static_cast<btCollisionObject*>(pair.m_pProxy0->m_clientObject);
            const auto* b=static_cast<btCollisionObject*>(pair.m_pProxy1->m_clientObject);
            const int kind=int(btSoftBody::upcast(a)!=nullptr)+int(btSoftBody::upcast(b)!=nullptr);
            const auto start=std::chrono::steady_clock::now();
            btCollisionDispatcher::defaultNearCallback(pair,dispatcher,info);
            collisionMs[kind]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            ++collisionCalls[kind];
        });
        for (int frame=0;frame<3;++frame) {
            std::printf("model phase=step frame=%d begin\n",frame);
            start=std::chrono::steady_clock::now();
            CProfileManager::Reset();
            for(int i=0;i<3;++i){collisionMs[i]=0;collisionCalls[i]=0;}
            host.world.stepSimulation(1.f/60,1,1.f/60);
            CProfileManager::dumpAll();
            for(int i=0;i<3;++i) std::printf("model collision soft_count=%d calls=%u ms=%.3f\n",i,collisionCalls[i],collisionMs[i]);
            const auto& sdf=host.world.getWorldInfo().m_sparsesdf;
            std::printf("model sdf cells=%d queries=%d probes=%d\n",sdf.ncells,sdf.nqueries,sdf.nprobes);
            const auto unique=UniqueSdfCells(sdf);
            std::printf("model sdf unique_cells=%zu\n",unique);
#if MMS_TEST_DX12
            Check(unique==std::size_t(sdf.ncells),"real model SDF cache does not duplicate cells");
#endif
            unsigned contacts=0;
            for (int i=0;i<host.world.getSoftBodyArray().size();++i) {
                const auto& body=*host.world.getSoftBodyArray()[i];
                contacts+=body.m_scontacts.size()+body.m_rcontacts.size();
                for (int j=0;j<body.m_nodes.size();++j)
                    Check(std::isfinite(body.m_nodes[j].m_x.length2()),"real model node remains finite");
            }
            std::printf("model phase=step frame=%d ms=%.3f contacts=%u\n",frame,elapsed(start),contacts);
        }
        const auto& status=soft_compute::GetStatus();
        std::printf("model cpu_steps=%llu gpu_steps=%llu detail=%s failures=%d\n",status.cpuSteps,status.gpuSteps,status.detail.c_str(),failures);
        DynamicsWorld::solveSoftBody=nullptr; states.clear();
        return failures ? 1 : 0;
    }
    Topology();
    if(argc==2 && !std::wcscmp(argv[1],L"--topology-only"))return failures?1:0;
    Qdef();Host host;if(!host.renderer->device)return 1;VertexColorShader(host);
#if MMS_TEST_DX12
    SdfCache(host);
    {
        const auto adapters=gpu::EnumerateAdapters();Check(!adapters.empty(),"hardware GPU available");
        Model model;Check(Load(host,model,Fixture(2,true)),"load GPU soft fixture");
        for(unsigned i=0;i<model.data->boneCount;++i){auto* matrix=model.data->boneTable[i].matInit;std::memset(matrix,0,64);for(int c=0;c<4;++c)matrix[c*5]=1;}
        pmx21::SyncSoftBodies(model.raw(),true);
        auto app=std::make_unique<MMDApp>();app->state=*std::make_unique<MMDAppState>();app->Renderer()=host.renderer.get();
        float world[16]{};for(int i=0;i<4;++i)world[i*5]=1;
        if(!adapters.empty()) {
            skinning::SetPreference({skinning::Mode::Adapter,adapters[0].id,adapters[0].name});
            for(int frame=0;frame<2;++frame) {
                host.world.stepSimulation(1.f/60,1,1.f/60);
                UpdateModelVertexBuffers(app.get(),model.raw(),world);
                Check(skinning::GetStatus().result==skinning::Result::Gpu,"soft model keeps GPU vertex processing");
                auto* vb=mdl::ResourceAs<IDirect3DVertexBuffer9>(model.data->vertexBuffer);void* p=nullptr;
                Check(SUCCEEDED(vb->Lock(0,0,&p,D3DLOCK_READONLY)),"read GPU soft output");
                if(p){const auto* v=static_cast<mdl::SkinnedVertexBase*>(p);Check(Near(v[1].position[1],host.world.getSoftBodyArray()[0]->m_nodes[1].m_x.y()),"GPU receives current simulated node each frame");vb->Unlock();}
            }
        }
        skinning::ForgetModel(model.raw());skinning::SetPreference({skinning::Mode::Cpu});app->Renderer()=nullptr;
    }
#endif
    for(int width:{1,2,4})for(bool utf8:{false,true})Integration(host,width,utf8);
    for(unsigned char shape:{0,1})for(int anchor:{0,1}) {
        Model model;Check(Load(host,model,Fixture(2,true,true,shape,anchor)),"read cloth/rope with normal/near anchor");
        for(unsigned i=0;i<model.data->boneCount;++i){auto* matrix=model.data->boneTable[i].matInit;std::memset(matrix,0,64);for(int c=0;c<4;++c)matrix[c*5]=1;}
        pmx21::SyncSoftBodies(model.raw(),true);
        if(host.world.getSoftBodyArray().size()) {
            auto* body=host.world.getSoftBodyArray()[0];
            Check(body->m_anchors.size()==1,"near anchors retain Bullet anchor constraint");
            Check(shape ? body->m_faces.size()==0&&body->m_links.size()==2 : body->m_faces.size()==1,"cloth and rope build different topology");
            pmx21::SyncSoftBodies(model.raw(),false);
#if MMS_TEST_DX12
            const auto adapters=gpu::EnumerateAdapters();
            if(!adapters.empty()) {
                auto app=std::make_unique<MMDApp>();app->state=*std::make_unique<MMDAppState>();app->Renderer()=host.renderer.get();
                float matrix[16]{};for(int i=0;i<4;++i)matrix[i*5]=1;
                // A rotated rope has no face normals: its normals must retain
                // bone deformation while positions come from the simulation.
                for(unsigned i=0;i<model.data->boneCount;++i) {
                    auto* bone=model.data->boneTable[i].matInit;
                    bone[0]=bone[10]=0;bone[2]=-1;bone[8]=1;
                }
                skinning::SetPreference({skinning::Mode::Cpu});
                UpdateModelVertexBuffers(app.get(),model.raw(),matrix);
                auto* vb=mdl::ResourceAs<IDirect3DVertexBuffer9>(model.data->vertexBuffer);
                mdl::SkinnedVertexBase reference[3]{};void* data=nullptr;
                if(SUCCEEDED(vb->Lock(0,0,&data,D3DLOCK_READONLY))){std::memcpy(reference,data,sizeof reference);vb->Unlock();}
                skinning::SetPreference({skinning::Mode::Adapter,adapters[0].id,adapters[0].name});
                UpdateModelVertexBuffers(app.get(),model.raw(),matrix);
                Check(skinning::GetStatus().result==skinning::Result::Gpu,"anchored cloth and rope retain GPU skinning");
                data=nullptr;
                if(SUCCEEDED(vb->Lock(0,0,&data,D3DLOCK_READONLY))) {
                    const auto* actual=static_cast<mdl::SkinnedVertexBase*>(data);
                    for(int i=0;i<3;++i)for(int c=0;c<3;++c)
                        Check(Near(actual[i].position[c],reference[i].position[c])&&Near(actual[i].normal[c],reference[i].normal[c]),"GPU soft position and normal match CPU");
                    vb->Unlock();
                } else Check(false,"read GPU soft normals");
                skinning::ForgetModel(model.raw());skinning::SetPreference({skinning::Mode::Cpu});app->Renderer()=nullptr;
            }
#endif
        } else Check(false,"create anchored soft body");
    }
    const auto good=Fixture(2,true);
    for(unsigned missing:{1u,2u,4u,18u,91u}){auto bad=good;bad.resize(bad.size()-missing);Model model;Check(!Load(host,model,bad),"reject truncated PMX 2.1 soft record");}
    Check(host.world.getNumCollisionObjects()==0&&host.world.getNumConstraints()==0,"failed loads remain disposable");
    std::printf("pmx21_failures=%d\n",failures);return failures?1:0;
}
