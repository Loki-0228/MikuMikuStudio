// MAINTENANCE: hardware-manual; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <chrono>
#include <cstdlib>
using namespace mikumikustudio;
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    skinning::SetPreference({skinning::Mode::Cpu});
    auto app=std::make_unique<MMDApp>(); g_Block=app.get();
    app->state=MMDAppState{};app->InitDefaults();
    if(!InitMainWindowAndD3D(app.get(),GetModuleHandleW(nullptr),SW_HIDE)) return 3;
    ShowWindow(app->MainWindow(),SW_HIDE);
    LoadModelFile(app.get(),argv[1],false);
    auto* model=app->ModelSlot(0); if(!model) return 4;
    auto& m=*mdl::Mdl(model);
    float world[16]{}; world[0]=world[5]=world[10]=world[15]=1;
    gpu::Skinning gpu;
    if(!gpu.Initialize(true)) {
        std::printf("debug initialization: %s\n",gpu.Error()); return 5;
    }
    std::printf("adapter=%ls vertices=%u bones=%u\n",gpu.AdapterName(),m.vertexCount,m.boneCount);
    for(unsigned pose=0;pose<5;++pose) {
        bone_scale::RestoreTransforms(model);
        for(int i=1;i<m.morphCount;++i)if(m.physicsMode!=2||mdl::Morphs(model)[i].type==1){
            mdl::Morphs(model)[i].value=float(pose)*.25f;break;
        }
        if(pose) for(unsigned i=0;i<m.boneCount;++i) {
            auto* mat=mdl::Bones(model)[i].matInit;
            const float a=float((i+pose)%17)*.03125f;
            std::memcpy(mat,world,64); mat[0]=std::cos(a);mat[1]=std::sin(a);mat[4]=-mat[1];mat[5]=mat[0];
            mat[12]=float(i%7)*.125f;mat[13]=float(i%5)*-.25f;
        }
        if(pose>=3 && m.boneCount) {
            bone_scale::SetPose(model,0,{{1.7f,.2f,0,0,.65f,.15f,0,0,1.2f}});
            if(pose==4 && m.boneCount>1)
                bone_scale::SetPose(model,1,{{.8f,0,.1f,.15f,1.6f,0,0,.2f,1.1f}});
            bone_scale::ApplyTransforms(model);
        }
        skinning::SetPreference({skinning::Mode::Cpu});
        UpdateModelVertexBuffers(app.get(),model,world);
        GpuModelInput input;
        if(!PrepareGpuModelInput(app.get(),model,world,input)) return 6;
        input.parameters.perVertex = pose != 0;
        input.parameters.geometryRevision = pose + 1;
        unsigned types[4]{};for(auto& v:input.vertices) ++types[v.type];
        gpu::Ticket ticket;
        if(!gpu.Submit(input.vertices,input.bones,input.materials,input.parameters,ticket)) {
            std::puts(gpu.Error());return 7;
        }
        std::vector<gpu::Output> actual;
        if(!gpu.Readback(ticket,actual)) { std::puts(gpu.Error());return 8; }
        for(unsigned uv=0;uv<=4;++uv) {
            std::vector<unsigned char> compact;
            if(!gpu.SubmitAndReadbackCompact(input.vertices,input.bones,input.materials,
                    input.parameters,uv,compact)) {std::puts(gpu.Error());return 20;}
            const unsigned body=32+16*uv,packedStride=body+16;
            if(compact.size()!=actual.size()*packedStride)return 21;
            for(std::size_t i=0;i<actual.size();++i) {
                if(std::memcmp(compact.data()+packedStride*i,&actual[i],body) ||
                   std::memcmp(compact.data()+packedStride*i+body,actual[i].edgePosition,16))return 22;
            }
            std::printf("PACKED_PARITY pose=%u uv=%u vertices=%zu bytes=%zu exact=1\n",pose,uv,actual.size(),compact.size());
        }
        if(pose==0 && std::getenv("MMS_PACKED_READBACK_BENCHMARK")) {
            input.parameters.geometryRevision=0; // production D3D9 upload policy
            const unsigned body=32+16*m.pmxAdditionalUvCount;
            std::vector<unsigned char> compact,bodyCopy(actual.size()*body),edgeCopy(actual.size()*16);
            for(unsigned block=0;block<4;++block) {
                const bool packed=block==1 || block==2;
                const auto run=[&] {
                    if(packed) {
                        if(!gpu.SubmitAndReadbackCompact(input.vertices,input.bones,input.materials,input.parameters,m.pmxAdditionalUvCount,compact))return false;
                    } else if(!gpu.SubmitAndReadback(input.vertices,input.bones,input.materials,input.parameters,actual))return false;
                    for(std::size_t i=0;i<actual.size();++i) {
                        const auto* src=packed ? compact.data()+i*(body+16) : reinterpret_cast<const unsigned char*>(&actual[i]);
                        std::memcpy(bodyCopy.data()+i*body,src,body);
                        std::memcpy(edgeCopy.data()+i*16,src+(packed ? body : 96),16);
                    }
                    return true;
                };
                for(unsigned warm=0;warm<8;++warm)if(!run())return 23;
                const auto start=std::chrono::steady_clock::now();unsigned frames=0;double elapsed=0;
                do {
                    if(!run())return 24;++frames;
                    elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                }while(elapsed<2);
                std::printf("PACKED_PERF block=%u compact=%u frames=%u milliseconds=%.6f readback_bytes=%zu\n",
                    block,unsigned(packed),frames,elapsed*1000/frames,actual.size()*(packed ? body+16 : 112));
            }
            input.parameters.geometryRevision=pose+1;
        }
        auto* main=mdl::ResourceAs<IDirect3DVertexBuffer9>(m.vertexBuffer);
        auto* edge=mdl::ResourceAs<IDirect3DVertexBuffer9>(m.vertexBuffer2);
        void* mainData=nullptr;void* edgeData=nullptr;
        if(FAILED(main->Lock(0,0,&mainData,0))) return 9;
        if(FAILED(edge->Lock(0,0,&edgeData,0))) {main->Unlock();return 9;}
        const unsigned stride=m.physicsMode==2 ? 32+16*m.pmxAdditionalUvCount : 32;
        double maximum=0;unsigned errors=0;
        for(unsigned i=0;i<m.vertexCount;++i) {
            const auto* expected=reinterpret_cast<const float*>(static_cast<const char*>(mainData)+stride*i);
            const auto& expectedEdge=static_cast<const mdl::EdgeVertex*>(edgeData)[i];
            const auto* got=reinterpret_cast<const float*>(&actual[i]);
            for(unsigned c=0;c<stride/4;++c) {
                const double delta=std::abs(double(got[c])-expected[c]); maximum=std::max(maximum,delta);
                if(!std::isfinite(got[c]) || delta>2e-5*std::max(1.,std::abs(double(expected[c])))) {
                    if(errors<4) std::printf("vertex=%u component=%u cpu=%.9g gpu=%.9g\n",i,c,expected[c],got[c]);
                    ++errors;
                }
            }
            for(unsigned c=0;c<3;++c) {
                const double delta=std::abs(double(actual[i].edgePosition[c])-expectedEdge.position[c]);maximum=std::max(maximum,delta);
                if(!std::isfinite(actual[i].edgePosition[c]) || delta>2e-5*std::max(1.,std::abs(double(expectedEdge.position[c])))) ++errors;
            }
            if(actual[i].edgeColor!=expectedEdge.diffuse) ++errors;
        }
        std::vector<unsigned char> referenceMain(stride * m.vertexCount);
        std::vector<mdl::EdgeVertex> referenceEdge(m.vertexCount);
        std::memcpy(referenceMain.data(), mainData, referenceMain.size());
        std::memcpy(referenceEdge.data(), edgeData, referenceEdge.size() * sizeof(mdl::EdgeVertex));
        edge->Unlock();main->Unlock();
        // Exercise the production D3D9/MME bridge, not only the compute kernel.
        for (const auto& adapter : skinning::AvailableAdapters()) {
            skinning::SetPreference({skinning::Mode::Adapter, adapter.id, adapter.name});
            UpdateModelVertexBuffers(app.get(), model, world);
            if (skinning::GetStatus().result != skinning::Result::Gpu ||
                skinning::GetStatus().gpuUpdates != 1 ||
                skinning::GetStatus().adapterName != adapter.name) return 14;
            if (mdl::ResourceAs<IDirect3DVertexBuffer9>(m.vertexBuffer) != main ||
                mdl::ResourceAs<IDirect3DVertexBuffer9>(m.vertexBuffer2) != edge) return 15;
            if (FAILED(main->Lock(0, 0, &mainData, 0))) return 16;
            if (FAILED(edge->Lock(0, 0, &edgeData, 0))) { main->Unlock(); return 16; }
            unsigned bridgeErrors = 0;
            for (unsigned v = 0; v < m.vertexCount; ++v) {
                const auto* expected = reinterpret_cast<const float*>(referenceMain.data() + stride * v);
                const auto* got = reinterpret_cast<const float*>(static_cast<const char*>(mainData) + stride * v);
                for (unsigned c = 0; c < stride / 4; ++c)
                    if (!std::isfinite(got[c]) || std::abs(double(got[c]) - expected[c]) >
                        2e-5 * std::max(1., std::abs(double(expected[c])))) {
                        if (bridgeErrors < 4) std::printf("bridge vertex=%u component=%u cpu=%.9g gpu=%.9g\n", v, c, expected[c], got[c]);
                        ++bridgeErrors;
                    }
                const auto& e = static_cast<const mdl::EdgeVertex*>(edgeData)[v];
                for (unsigned c = 0; c < 3; ++c)
                    if (!std::isfinite(e.position[c]) || std::abs(double(e.position[c]) - referenceEdge[v].position[c]) >
                        2e-5 * std::max(1., std::abs(double(referenceEdge[v].position[c])))) ++bridgeErrors;
                if (e.diffuse != referenceEdge[v].diffuse) ++bridgeErrors;
            }
            edge->Unlock(); main->Unlock();
            std::printf("bridge pose=%u adapter=%ls errors=%u buffer_identity=preserved\n",
                        pose, adapter.name.c_str(), bridgeErrors);
            if (bridgeErrors) return 17;
        }
        std::printf("pose=%u types=%u,%u,%u,%u max_abs_error=%.9g errors=%u\n",pose,types[0],types[1],types[2],types[3],maximum,errors);
        if(errors) return 10;
        // Recycle all three slots repeatedly; invalid types must not submit.
        for(unsigned i=0;i<7;++i)
            if(!gpu.Submit(input.vertices,input.bones,input.materials,input.parameters,ticket)) return 11;
        if(!gpu.Wait(ticket)) return 12;
        std::vector<gpu::Output> reused;
        if(!gpu.Readback(ticket,reused)||reused.size()!=actual.size()||
           std::memcmp(reused.data(),actual.data(),actual.size()*sizeof(gpu::Output))!=0) return 13;
        const auto retired=ticket;
        auto* initializedDevice=gpu.Device();
        if(!gpu.ReleaseWorkingBuffers() || gpu.OutputResource(retired) ||
           gpu.Device()!=initializedDevice) return 18;
        if(!gpu.SubmitAndReadback(input.vertices,input.bones,input.materials,input.parameters,reused,&ticket) ||
           ticket.fence==retired.fence || gpu.OutputResource(retired) || reused.size()!=actual.size() ||
           std::memcmp(reused.data(),actual.data(),actual.size()*sizeof(gpu::Output))!=0) return 19;
    }
    ResetAppState(app.get());ShutdownCleanup(app.get());g_Block=nullptr;
    std::puts("PASS D3D12 skinning, outline, UV and submission reuse");
}
