// MAINTENANCE: hardware-manual. Hidden D3D9 buffers, no visible window.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/native_shadow_projection.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace mikumikustudio {
bool UpdateModelVertexBuffersCpu(MMDApp*, unsigned char*, const float[16]);
}
using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
void Check(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL %s\n", message); std::exit(1); }
}
std::vector<unsigned char> Read(IDirect3DVertexBuffer9* buffer) {
    D3DVERTEXBUFFER_DESC desc{}; Check(SUCCEEDED(buffer->GetDesc(&desc)), "buffer description");
    void* data=nullptr;Check(SUCCEEDED(buffer->Lock(0,0,&data,D3DLOCK_READONLY)), "read buffer");
    std::vector<unsigned char> result(desc.Size);std::memcpy(result.data(),data,desc.Size);buffer->Unlock();return result;
}
int main() {
    HWND window=CreateWindowW(L"STATIC",L"Vertex cache tests",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window!=nullptr,"hidden window");
    ComPtr<IDirect3D9> d3d;d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));Check(bool(d3d),"D3D9");
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    Check(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device)),"device");
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
    app->CameraDistance()=-20;app->CameraFov()=30;
    skinning::SetPreference({skinning::Mode::Cpu});
    unsigned cases=0;
    for(bool loaded : {false,true}) for(int uv=-1;uv<=5;++uv) {
        const unsigned count=uv==5?32771:16;
        auto m=std::make_unique<mdl::ModelRecord>();auto* model=reinterpret_cast<unsigned char*>(m.get());
        std::vector<mdl::PmxVertex> pmx(count);std::vector<mdl::PmdVertex> pmd(count);
        std::array<mdl::BoneRecord,4> bones{};mdl::ModelMaterialRecord material{};
        std::array<mdl::MorphRecord,7> morphs{};
        mdl::PmdVertexMorphEntry base[]={{0,{0,0,0}},{1,{.25f,0,0}}}, offsets[]={{0,{1,2,3}},{0,{-.25f,.5f,-1}}};
        mdl::PmxGroupMorphEntry group[]={{1,.5f},{2,-.25f}};
        std::array<mdl::PmxUvMorphEntry,5> uvBase{},uvDelta{};
        float world[16]{};world[0]=world[5]=world[10]=world[15]=1;
        for(unsigned b=0;b<4;++b){std::memcpy(bones[b].matInit,world,64);bones[b].matInit[12]=b*.125f;}
        material.edgeSize=1.5f;material.edgeColor[0]=.4f;material.edgeColor[3]=.5f;
        for(unsigned i=0;i<count;++i) {
            pmd[i].position[0]=pmx[i].position[0]=i*.25f;pmd[i].normal[1]=pmx[i].normal[1]=1;
            pmd[i].bone[1]=1;pmd[i].weightPercent=70;
            pmx[i].weightType=static_cast<mdl::PmxWeightType>(i%4);pmx[i].weight[0]=.4f;
            pmx[i].edgeScale=.5f;pmx[i].hasPmxEdgeData=1;
            for(unsigned b=0;b<4;++b){pmx[i].bone[b]=b;if(i%4==2)pmx[i].weight[b]=.25f;}
        }
        m->loadComplete=1;m->physicsMode=uv<0?0:2;m->pmxAdditionalUvCount=uv<0?0:std::min(uv,4);
        m->vertexCount=count;m->boneCount=4;m->materialCount=1;m->edgeScale=1;
        m->pmxVertices=pmx.data();m->rawVertices=pmd.data();m->boneTable=bones.data();m->materials=&material;
        m->morphs=morphs.data();m->morphCount=7;
        mdl::BaseVertexMorphTable(model)=base;mdl::BaseVertexMorphCount(model)=2;
        morphs[0].type=0;morphs[0].groupEntries=group;morphs[0].groupCount=2;
        morphs[1].type=1;morphs[1].vertexEntries=offsets;morphs[1].offsetCount=2;
        for(unsigned i=0;i<5;++i) {
            uvBase[i].vertexIndex=uvDelta[i].vertexIndex=0;uvDelta[i].offset[0]=.2f;
            mdl::UvMorphTables(model).byFamily[i]=&uvBase[i];mdl::UvMorphCounts(model).byFamily[i]=1;
            morphs[i+2].type=3+i;morphs[i+2].uvEntries[i]=&uvDelta[i];morphs[i+2].uvCounts[i]=1;
        }
        const unsigned stride=32+16*m->pmxAdditionalUvCount;
        ComPtr<IDirect3DVertexBuffer9> body,edge,replacement;
        Check(SUCCEEDED(device->CreateVertexBuffer(count*stride,0,0,D3DPOOL_MANAGED,&body,nullptr)),"body allocation");
        Check(SUCCEEDED(device->CreateVertexBuffer(count*16,0,0,D3DPOOL_MANAGED,&edge,nullptr)),"edge allocation");
        for(auto* buffer:{body.Get(),edge.Get()}){void* data=nullptr;D3DVERTEXBUFFER_DESC desc{};buffer->GetDesc(&desc);buffer->Lock(0,0,&data,0);std::memset(data,0,desc.Size);buffer->Unlock();}
        m->vertexBuffer=body.Get();m->vertexBuffer2=edge.Get();
        if(loaded)model_vertex_cache::RegisterLoaded(model);
        const auto update=[&]{UpdateModelVertexBuffers(app.get(),model,world);};
        const auto compare=[&] {
            const auto actual=Read(static_cast<IDirect3DVertexBuffer9*>(m->vertexBuffer)),outline=Read(edge.Get());
            Check(UpdateModelVertexBuffersCpu(app.get(),model,world),"uncached reference");
            Check(actual==Read(static_cast<IDirect3DVertexBuffer9*>(m->vertexBuffer))&&outline==Read(edge.Get()),"cached bytes equal uncached body and outline");
            ++cases;
        };
        const auto warm=[&] {
            update();update();const auto before=model_vertex_cache::GetStats();update();
            Check(model_vertex_cache::GetStats().hits==before.hits+1,"unchanged model reuses completed buffers");
        };
        const auto changed=[&](auto mutate) {
            warm();const auto before=model_vertex_cache::GetStats();mutate();update();
            Check(model_vertex_cache::GetStats().hits==before.hits,"changed input cannot hit cache");compare();
        };
        const auto sourceChanged=[&](auto mutate) {
            changed([&]{if(loaded)model_vertex_cache::Invalidate(model);mutate();});
        };
        warm();compare();
        warm();
        {
            const auto bytes=Read(body.Get());
            const auto bounds=NativeShadowVertexBounds(bytes.data(),stride,count);
            NativeShadowBounds restored;
            model_vertex_cache::RememberShadowBounds(model,body.Get(),stride,count,bounds);
            Check(model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored)==loaded,
                "only registered host geometry caches shadow bounds");
            if(loaded){
                Check(restored.minimum==bounds.minimum&&restored.maximum==bounds.maximum,"bounds equal full vertex scan");
                model_vertex_cache::BeginFrame();
                Check(!model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored),"new render requires a pose check");
                update();
                Check(model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored),"verified unchanged body reuses previous bound");
                Check(!model_vertex_cache::ShadowBounds(model,body.Get(),stride+4,count,restored)&&
                    !model_vertex_cache::ShadowBounds(model,body.Get(),stride,count-1,restored)&&
                    !model_vertex_cache::ShadowBounds(model,edge.Get(),stride,count,restored),"bound requires identical buffer and layout");
                model_vertex_cache::SetShadowBoundsEnabledForTests(false);
                Check(!model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored),"bound cache comparison control");
                model_vertex_cache::SetShadowBoundsEnabledForTests(true);
                model_vertex_cache::RememberShadowBounds(model,body.Get(),stride,count,bounds);
                bones[0].matInit[12]+=.125f;model_vertex_cache::BeginFrame();update();
                Check(!model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored),"bone change invalidates shadow bounds");
                warm();
                const auto movedBytes=Read(body.Get());
                const auto moved=NativeShadowVertexBounds(movedBytes.data(),stride,count);
                model_vertex_cache::RememberShadowBounds(model,body.Get(),stride,count,moved);
                Check(model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored)&&
                    restored.minimum==moved.minimum&&restored.maximum==moved.maximum,"new pose gets freshly scanned bounds");
                model_vertex_cache::Invalidate(model);
                Check(!model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored),"geometry edit invalidates shadow bounds");
                warm();model_vertex_cache::RememberShadowBounds(model,body.Get(),stride,count,moved);
                model_vertex_cache::Clear();
                Check(!model_vertex_cache::ShadowBounds(model,body.Get(),stride,count,restored),"device reset clears bounds");
            }
        }
        changed([&]{bones[0].matInit[12]+=.3f;});
        changed([&]{bones[1].matInit[0]=1.25f;bones[1].matInit[4]=.125f;});
        changed([&]{morphs[1].value=.7f;});
        sourceChanged([&]{offsets[0].offset[1]+=.25f;});
        sourceChanged([&]{base[0].offset[2]+=.25f;});
        changed([&]{morphs[1].value=0;});
        changed([&]{app->CameraDistance()-=3;});
        changed([&]{app->CameraFov()+=5;});
        changed([&]{world[13]+=.5f;});
        changed([&]{m->edgeScale+=.25f;});
        sourceChanged([&]{pmx[4].normal[0]=pmd[4].normal[0]=.25f;});
        sourceChanged([&]{pmx[count-1].position[0]+=1;pmd[count-1].position[0]+=1;});
        sourceChanged([&]{pmx[4].uv[0]=pmd[4].uv[0]=.75f;});
        sourceChanged([&]{pmx[4].hasPmxEdgeData=0;pmd[4].edgeDisabled=1;});
        if(uv>=0) {
            changed([&]{material.edgeColor[0]+=.1f;});changed([&]{material.edgeSize+=.2f;});
            changed([&]{app->state.selfShadowEnabled=!app->state.selfShadowEnabled;});
            changed([&]{morphs[0].value=.5f;});sourceChanged([&]{group[0].weight=-.2f;});
            for(unsigned i=0;i<5;++i){changed([&]{morphs[i+2].value=.5f;});sourceChanged([&]{uvDelta[i].offset[1]+=.3f;});sourceChanged([&]{uvBase[i].offset[0]+=.2f;});}
        }
        if(loaded) {
            ComPtr<IDirect3DVertexBuffer9> referenceBody,referenceEdge;
            Check(SUCCEEDED(device->CreateVertexBuffer(count*stride,0,0,D3DPOOL_MANAGED,&referenceBody,nullptr)),"reference body");
            Check(SUCCEEDED(device->CreateVertexBuffer(count*16,0,0,D3DPOOL_MANAGED,&referenceEdge,nullptr)),"reference edge");
            // Preserve fields the PMD worker intentionally does not overwrite.
            for(auto pair:{std::make_pair(body.Get(),referenceBody.Get()),std::make_pair(edge.Get(),referenceEdge.Get())}) {
                const auto bytes=Read(pair.first);void* target=nullptr;
                Check(SUCCEEDED(pair.second->Lock(0,0,&target,0)),"seed reference");
                std::memcpy(target,bytes.data(),bytes.size());pair.second->Unlock();
            }
            warm();
            for(unsigned frame=0;frame<16;++frame) {
                app->CameraDistance()-=.125f;app->CameraFov()+=.25f;world[13]+=.015625f;
                if(uv>=0){material.edgeSize+=.015625f;material.edgeColor[0]-=.00390625f;app->state.selfShadowEnabled^=1;}
                const auto before=model_vertex_cache::GetStats();update();
                if(frame>0)Check(model_vertex_cache::GetStats().outlineHits==before.outlineHits+1,"camera motion updates only outline");
                auto reference=std::make_unique<mdl::ModelRecord>(*m);
                reference->vertexBuffer=referenceBody.Get();reference->vertexBuffer2=referenceEdge.Get();
                Check(UpdateModelVertexBuffersCpu(app.get(),reinterpret_cast<unsigned char*>(reference.get()),world),"independent full reference");
                Check(Read(body.Get())==Read(referenceBody.Get())&&Read(edge.Get())==Read(referenceEdge.Get()),"moving outline bytes equal complete update");
                ++cases;
            }
            Check(model_vertex_cache::GetStats().cpuBytes==count*sizeof(model_vertex_cache::OutlineVertex),"only stable CPU outline input snapshot retained");
            const auto before=model_vertex_cache::GetStats();bones[0].matInit[12]+=.25f;update();
            Check(model_vertex_cache::GetStats().outlineHits==before.outlineHits&&model_vertex_cache::GetStats().cpuBytes==0,"bone motion drops outline snapshot");compare();
        }
        changed([&]{Check(SUCCEEDED(device->CreateVertexBuffer(count*stride,0,0,D3DPOOL_MANAGED,&replacement,nullptr)),"replacement allocation");m->vertexBuffer=replacement.Get();});
        warm();skinning::ForgetModel(model);const auto before=model_vertex_cache::GetStats();update();
        Check(model_vertex_cache::GetStats().hits==before.hits,"reload invalidates cache");compare();
        warm();skinning::SetPreference({skinning::Mode::Cpu});const auto switched=model_vertex_cache::GetStats();update();
        Check(model_vertex_cache::GetStats().hits==switched.hits,"processor change invalidates cache");compare();
        skinning::ForgetModel(model);Check(model_vertex_cache::GetStats().bytes==0&&model_vertex_cache::GetStats().cpuBytes==0,"disposing model releases snapshots");
    }
    Check(!IsWindowVisible(window),"test stays hidden");DestroyWindow(window);
    std::printf("PASS vertex cache: %u byte-exact reference cases, PMD and PMX UV0-4\n",cases);
}
