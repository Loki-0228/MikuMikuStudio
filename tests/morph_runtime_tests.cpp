// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <vector>

using namespace mikumikustudio;
static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static void CheckParallelGeometryMorphs() {
    // Non-divisible ranges, duplicate vertex entries, all five UV families,
    // grouped references and invalid indices exercise ownership and ordering.
    constexpr unsigned count=65539, fields=21;
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
    auto record=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::PmxVertex> vertices(count);
    std::vector<mdl::PmdVertexMorphEntry> base(count),offsets(count+3);
    std::array<std::vector<mdl::PmxUvMorphEntry>,5> uvBase,uvOffsets;
    std::array<mdl::MorphRecord,7> morphs{};
    mdl::PmxGroupMorphEntry refs[]={{0,.5f},{0,-.25f},{1,.75f},{5,-.5f},{-1,1},{99,1}};
    mdl::BoneRecord bone{};mdl::ModelMaterialRecord material{};
    float world[16]{};world[0]=world[5]=world[10]=world[15]=1;
    std::memcpy(bone.matInit,world,sizeof(world));
    record->vertexCount=count;record->boneCount=record->materialCount=1;
    record->loadComplete=1;record->physicsMode=2;record->pmxAdditionalUvCount=4;
    record->pmxVertices=vertices.data();record->boneTable=&bone;record->materials=&material;
    record->morphs=morphs.data();record->morphCount=unsigned(morphs.size());
    auto* model=reinterpret_cast<unsigned char*>(record.get());
    mdl::BaseVertexMorphTable(model)=base.data();mdl::BaseVertexMorphCount(model)=count;
    morphs[0].type=1;morphs[0].vertexEntries=offsets.data();morphs[0].offsetCount=unsigned(offsets.size());
    morphs[6].type=0;morphs[6].groupEntries=refs;morphs[6].groupCount=6;
    for(unsigned i=0;i<count;++i) {
        base[i].vertexIndex=i;offsets[i].vertexIndex=i;
        for(unsigned c=0;c<3;++c){base[i].offset[c]=float(i%37)*.125f;offsets[i].offset[c]=float(int(i%11)-5)*.0625f;}
    }
    offsets[count]={count/2,{16777216.f,1,2}};
    offsets[count+1]={count/2,{-16777216.f,-1,3}};
    offsets[count+2]={count+100,{1,2,3}};
    for(unsigned family=0;family<5;++family) {
        uvBase[family].resize(count);uvOffsets[family].resize(count+3);
        for(unsigned i=0;i<count;++i) {
            auto& b=uvBase[family][i];auto& d=uvOffsets[family][i];b.vertexIndex=d.vertexIndex=int(i);
            for(unsigned c=0;c<4;++c){b.offset[c]=float((i+c)%31)*.125f;d.offset[c]=float(int((i+c)%7)-3)*.0625f;}
        }
        uvOffsets[family][count]={int(count/2),{16777216.f,1,2,3}};
        uvOffsets[family][count+1]={int(count/2),{-16777216.f,-1,-2,-3}};
        uvOffsets[family][count+2]={-1,{1,1,1,1}};
        mdl::UvMorphTables(model).byFamily[family]=uvBase[family].data();
        mdl::UvMorphCounts(model).byFamily[family]=count;
        auto& morph=morphs[family+1];morph.type=3+family;
        morph.uvEntries[family]=uvOffsets[family].data();morph.uvCounts[family]=count+3;
    }
    GpuModelInput actual;
    for(unsigned pose=0;pose<6;++pose) {
        const float direct=pose==0||pose==5?0.f:float(int(pose)-2)*.25f;
        const float group=pose==0||pose==5?0.f:float(pose)*-.125f;
        for(unsigned i=0;i<6;++i)morphs[i].value=direct;
        morphs[6].value=group;
        std::vector<float> expected(std::size_t(count)*fields);
        for(unsigned i=0;i<count;++i) {
            auto* p=expected.data()+std::size_t(i)*fields;
            std::memcpy(p,base[i].offset,12);
            for(unsigned f=0;f<5;++f)std::memcpy(p+(f?5+(f-1)*4:3),uvBase[f][i].offset,(f?4:2)*4);
        }
        const auto accumulate=[&](unsigned morph,float weight,float factor) {
            if(morph==0)for(const auto& e:offsets){if(e.vertexIndex>=count)continue;
                for(unsigned c=0;c<3;++c)expected[std::size_t(e.vertexIndex)*fields+c]+=e.offset[c]*weight*factor;}
            else for(const auto& e:uvOffsets[morph-1]){if(e.vertexIndex<0||unsigned(e.vertexIndex)>=count)continue;
                for(unsigned c=0;c<(morph==1?2u:4u);++c)
                    expected[std::size_t(e.vertexIndex)*fields+(morph==1?3:5+(morph-2)*4)+c]+=e.offset[c]*weight*factor;}
        };
        if(direct!=0)for(unsigned morph=0;morph<6;++morph)accumulate(morph,direct,1);
        if(group!=0){accumulate(0,group,.5f);accumulate(0,group,-.25f);accumulate(1,group,.75f);accumulate(5,group,-.5f);}
        Check(PrepareGpuModelInput(app.get(),model,world,actual,true),"prepare large parallel geometry morph fixture");
        for(unsigned i=0;i<count;++i) {
            const auto* p=expected.data()+std::size_t(i)*fields;const auto& v=actual.vertices[i];
            Check(!std::memcmp(p,v.position,12)&&!std::memcmp(p+3,v.uv,8),"parallel position and UV results retain serial bits");
            for(unsigned f=0;f<4;++f)Check(!std::memcmp(p+5+f*4,v.additionalUv[f],16),"parallel additional UV retains serial bits");
        }
    }
    std::puts("PASS parallel morph geometry: 6 poses x 65539 vertices, duplicate/group/UV/reset coverage");
}

int main() {
    CheckParallelGeometryMorphs();
    auto model = std::make_unique<mdl::ModelRecord>();
    auto* bytes = reinterpret_cast<unsigned char*>(model.get());
    std::array<mdl::BoneRecord, 128> bones{};
    std::array<mdl::MorphRecord, 3> morphs{};
    std::array<mdl::PmxBoneMorphEntry, 2> offsets{};
    // Only two entries, both referencing the final bone. The old allocation
    // held two records but the physics pass wrote record 127 into another heap
    // allocation. Repeated entries must also accumulate/apply only once.
    for (auto& entry : offsets) {
        entry.boneIndex = 127;
        entry.translation[0] = 2.f;
        entry.rotation[3] = 1.f;
    }
    mdl::PmxGroupMorphEntry refs[] = {{0, .5f}, {-1, 1.f}, {99, 1.f}};
    mdl::PmxBoneMorphEntry invalid{};
    invalid.boneIndex = 128; invalid.rotation[3] = 1.f;
    morphs[0].type = 2; morphs[0].value = .5f;
    morphs[0].boneCount = 2; morphs[0].boneEntries = offsets.data();
    morphs[1].type = 0; morphs[1].value = .5f;
    morphs[1].groupCount = 3; morphs[1].groupEntries = refs;
    morphs[2].type = 2; morphs[2].value = 1.f;
    morphs[2].boneCount = 1; morphs[2].boneEntries = &invalid;
    model->boneCount = static_cast<unsigned>(bones.size());
    model->boneTable = bones.data();
    model->morphs = morphs.data(); model->morphCount = 3;
    model->physicsMode = 2;
    model->maxBoneLayer = -1; // Isolate accumulation from the transform/IK solver.
    for (auto& bone : bones) bone.rotQuat[3] = 1.f;
    for (int reload = 0; reload < 20; ++reload) {
        InitializeBoneMorphOffsets(bytes); // Same allocation path as LoadPMX.
        Check(mdl::BoneMorphOffsetCount(bytes) == 128, "work pool covers every bone index");
        for (int frame = 0; frame < 4; ++frame) {
            ModelApplyMorphs(bytes);
            SetPhysicsMode(bytes, 0, nullptr, 0);
            Check(std::abs(bones[127].physicsOffset[0] - 3.f) < 1e-6f,
                  "direct and grouped morphs combine once on a sparse high-index bone");
            Check(bones[0].physicsOffset[0] == 0.f && bones[126].physicsOffset[0] == 0.f,
                  "unaffected bones stay at rest");
            Check(bones[127].physicsQuat[3] == 1.f, "identity rotation survives repeated frames");
        }
    }
    morphs[0].value = morphs[1].value = morphs[2].value = 0.f;
    ModelApplyMorphs(bytes); SetPhysicsMode(bytes, 0, nullptr, 0);
    Check(bones[127].physicsOffset[0] == 0.f, "zero morph weights reset previous offsets");
    model->morphCount = 0;
    InitializeBoneMorphOffsets(bytes);
    Check(!mdl::BoneMorphOffsets(bytes) && mdl::BoneMorphOffsetCount(bytes) == 0,
          "reload without bone morphs clears old work storage");
    std::puts("Bone morph runtime regressions passed");
}
