// Regression against the actual private playback implementation. The paired
// before/after builds each include their own production file, never a copy.
#include "../src/render/native_mesh.cpp"
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <chrono>
#include <cstdio>
#include <functional>

namespace {
namespace Abc=Alembic::Abc;
namespace Geom=Alembic::AbcGeom;
namespace native=mikumikustudio::native_mesh;
namespace cache=mikumikustudio::mesh_bake_cache;
namespace data=mikumikustudio::mesh_data;
using mikumikustudio::MMDApp;
using mikumikustudio::deformed_mesh_capture::Mesh;

void Require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}

struct Part {
    std::vector<uint32_t> ids;
    bool mapped=true,patch=false;
    size_t vertices=3;
};

void Archive(const std::filesystem::path& path,const std::vector<Part>& parts){
    std::ofstream stream(path,std::ios::binary);
    Require(bool(stream),"open ABC fixture");
    {
        Alembic::AbcCoreAbstract::MetaData metadata;
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive()(&stream,metadata));
        const auto sampling=archive.addTimeSampling(Abc::TimeSampling(1./30.,0.));
        for(size_t material=0;material<parts.size();++material){
            const auto& part=parts[material];
            Geom::OPolyMesh shape(archive.getTop(),"model_0_material_"+std::to_string(material)+"_shader_0_pass_0",sampling);
            const auto properties=shape.getSchema().getUserProperties();
            if(part.patch)Abc::OBoolProperty(properties,"mdsVertexPatch").set(true);
            if(part.mapped)Abc::OUInt32ArrayProperty(properties,"mdsSourceVertices",sampling).set(Abc::UInt32ArraySample(part.ids));
            std::vector<Abc::V3f> positions(part.vertices),normals(part.vertices,Abc::V3f(0,0,-1));
            std::vector<Abc::V2f> uvs(part.vertices,Abc::V2f(.25f,.75f));
            std::vector<int32_t> indices(part.vertices),counts(part.vertices/3,3);
            for(size_t i=0;i<part.vertices;++i){positions[i]=Abc::V3f(float(i)+10,float(i%3),0);indices[i]=int32_t(i);}
            Geom::OPolyMeshSchema::Sample sample{Abc::P3fArraySample(positions),
                Abc::Int32ArraySample(indices),Abc::Int32ArraySample(counts)};
            sample.setUVs(Geom::OV2fGeomParam::Sample(Abc::V2fArraySample(uvs),Geom::kVertexScope));
            sample.setNormals(Geom::ON3fGeomParam::Sample(Abc::N3fArraySample(normals),Geom::kVertexScope));
            shape.getSchema().set(sample);
        }
    }
    stream.close();Require(bool(stream),"finish ABC fixture");
}

native::Snapshot Source(const void* object,size_t count=4){
    native::Snapshot source;
    source.object.id=reinterpret_cast<uintptr_t>(object);source.object.visible=1;
    data::Identity(source.object.world);source.mesh.vertices.resize(count);source.mesh.materials.resize(2);
    for(size_t i=0;i<count;++i){auto& vertex=source.mesh.vertices[i];
        vertex.position[0]=float(i);vertex.normal[2]=1;vertex.uv[0]=.9f;
        vertex.sourceVertex=uint32_t(i);vertex.edgeScale=.25f*float(i+1);vertex.flags=i%2?0:data::kEdgeDisabled;
        for(size_t c=0;c<16;++c)vertex.additionalUv[c]=float(i*16+c+1)*.125f;
    }
    source.mesh.triangles.push_back({{0,1,2},0});return source;
}

void Attributes(const data::Vertex& vertex,const data::Vertex* source){
    for(size_t c=0;c<16;++c)Require(vertex.additionalUv[c]==(source?source->additionalUv[c]:0.f),"additional UV survives source mapping");
    Require(vertex.edgeScale==(source?source->edgeScale:1.f),"edge scale follows source or documented default");
    Require(vertex.flags==(source?source->flags:0u),"edge flag follows source or documented default");
    Require(vertex.sourceVertex==(source?source->sourceVertex:UINT32_MAX),"source identity survives playback");
}

struct Scene {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    unsigned char object[64]{};
    Scene(){app->state=mikumikustudio::MMDAppState{};app->ModelSlot(0)=object;}
    ~Scene(){cache::Clear();}
    void Install(const std::filesystem::path& path){
        cache::Clear();std::string error;Require(cache::Install(*app,path,error),"install fixture: "+error);
    }
};

void Roundtrip(Scene& scene,const std::filesystem::path& directory,const char* name,
               const std::vector<Part>& parts){
    const auto path=directory/(std::string(name)+".abc");Archive(path,parts);scene.Install(path);
    const auto original=Source(scene.object);auto replay=original;
    native::ApplyBakedMesh(replay,0);
    size_t offset=0;
    for(const auto& part:parts)for(size_t i=0;i<part.vertices;++i){
        const auto id=part.mapped?part.ids[i]:UINT32_MAX;
        const auto& vertex=replay.mesh.vertices.at(offset++);
        Attributes(vertex,id==UINT32_MAX?nullptr:&original.mesh.vertices[id]);
        Require(vertex.position[0]==float(i)+10&&vertex.uv[0]==.25f&&vertex.uv[1]==.25f,
                "cached positions and base UV override source fields");
    }
    Require(replay.mesh.vertices.size()==offset,"all material parts survive playback");
    Require(original.mesh.vertices[0].position[0]==0,"source snapshot remains unchanged");
}

void MappingPresence(Scene& scene,const std::filesystem::path& directory){
    for(bool mapped:{false,true}){
        const auto path=directory/(mapped?L"identity.abc":L"legacy.abc");
        Archive(path,{{{0,1,2},mapped,false,3}});scene.Install(path);
        std::vector<Mesh> samples;std::string error;Require(cache::Sample(scene.object,0,samples,error),error);
        Require(samples.size()==1,"one sample returned");
        Require(mapped?samples[0].sourceVertices==std::vector<uint32_t>({0,1,2}):samples[0].sourceVertices.empty(),
                "missing source property is distinct from explicit identity");
    }
}

void Rejected(Scene& scene,const std::filesystem::path& directory,const char* name,
              const std::vector<Part>& parts){
    const auto path=directory/(std::string(name)+".abc");Archive(path,parts);scene.Install(path);
    const auto original=Source(scene.object);auto replay=original;bool rejected=false;
    try{native::ApplyBakedMesh(replay,0);}catch(const std::exception&){rejected=true;}
    Require(rejected,"invalid source map must be rejected");
    Require(replay.mesh.vertices.size()==original.mesh.vertices.size()&&replay.mesh.triangles.size()==original.mesh.triangles.size(),
            "rejection preserves snapshot geometry counts");
    for(size_t i=0;i<original.mesh.vertices.size();++i){
        const auto& before=original.mesh.vertices[i];const auto& after=replay.mesh.vertices[i];
        Require(!std::memcmp(&before,&after,sizeof(before)),"source-map validation finishes before any vertex is changed");
    }
}

void Partial(Scene& scene,const std::filesystem::path& directory){
    const auto path=directory/L"partial.abc";Archive(path,{{{2,UINT32_MAX,0},true,true,3}});scene.Install(path);
    const auto original=Source(scene.object);auto replay=original;native::ApplyBakedMesh(replay,0);
    Require(replay.mesh.vertices.size()==4&&replay.mesh.vertices[2].position[0]==10&&replay.mesh.vertices[0].position[0]==12,
            "partial cache only addresses selected original rows");
    Require(replay.mesh.vertices[1].position[0]==1,"partial sentinel leaves unselected row unchanged");
    for(size_t i=0;i<4;++i)Attributes(replay.mesh.vertices[i],&original.mesh.vertices[i]);
}

void Benchmark(Scene& scene,const std::filesystem::path& directory){
    constexpr size_t vertices=50001;constexpr unsigned warmup=10,iterations=100;
    Part part;part.vertices=vertices;part.ids.resize(vertices);
    for(size_t i=0;i<vertices;++i)part.ids[i]=uint32_t(vertices-1-i);
    const auto path=directory/L"replay-benchmark.abc";Archive(path,{part});scene.Install(path);
    const auto original=Source(scene.object,vertices);double elapsed=0,checksum=0;
    for(unsigned i=0;i<warmup+iterations;++i){
        auto replay=original; // Source capture preparation is outside this timer.
        const auto begin=std::chrono::steady_clock::now();native::ApplyBakedMesh(replay,double(i%3)/30.);
        const auto end=std::chrono::steady_clock::now();
        if(i>=warmup){elapsed+=std::chrono::duration<double,std::milli>(end-begin).count();checksum+=replay.mesh.vertices[0].additionalUv[0];}
    }
    // Print correctness evidence without stopping the old implementation before
    // its timing can be captured; the dedicated regression asserts correctness.
    std::printf("BAKE_REPLAY_BENCHMARK warmup=%u iterations=%u vertices=%zu elapsed_ms=%.6f mean_ms=%.6f abc_bytes=%llu attribute_checksum=%.6f expected_checksum=%.6f\n",
        warmup,iterations,vertices,elapsed,elapsed/iterations,
        static_cast<unsigned long long>(std::filesystem::file_size(path)),checksum,
        double(original.mesh.vertices.back().additionalUv[0])*iterations);
}
}

int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Require(argc==2||(argc==3&&std::wstring(argv[2])==L"--benchmark"),"output directory required, optional --benchmark");
        // The application loads D3DX during renderer startup. This CPU-only
        // entry calls playback directly, so initialize the same matrix API.
        auto& d3dx=mikumikustudio::d3dx::Get();
        Require(d3dx.Load()&&d3dx.inverse&&d3dx.vec3Transform,"load production D3DX matrix entry points");
        const auto directory=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(directory);
        Scene scene;if(argc==3){Benchmark(scene,directory);return 0;}
        unsigned failed=0;
        const auto run=[&](const char* name,const std::function<void()>& action){
            std::printf("RUN %s\n",name);
            try{action();std::printf("PASS %s\n",name);}catch(const std::exception& error){++failed;std::printf("FAIL %s: %s\n",name,error.what());}
        };
        run("cross-material reorder and duplicate",[&]{Roundtrip(scene,directory,"reorder",{{{2,0,1}},{{2,3,0}}});});
        run("explicit identity",[&]{Roundtrip(scene,directory,"identity-values",{{{0,1,2}}});});
        run("mapping presence",[&]{MappingPresence(scene,directory);});
        run("legacy defaults",[&]{Roundtrip(scene,directory,"legacy-values",{{{},false,false,3}});});
        run("generated defaults",[&]{Roundtrip(scene,directory,"generated",{{{2,UINT32_MAX,1}}});});
        run("wrong map length",[&]{Rejected(scene,directory,"short",{{{0,1}}});});
        run("out-of-range source",[&]{Rejected(scene,directory,"range",{{{0,1,2}},{{2,4,0}}});});
        run("partial mapping required",[&]{Rejected(scene,directory,"partial-missing",{{{},false,true,3}});});
        run("partial range validation is atomic",[&]{Rejected(scene,directory,"partial-range",{{{0,1,2},true,true,3},{{2,4,0},true,true,3}});});
        run("partial attributes",[&]{Partial(scene,directory);});
        std::printf("native bake attributes: %u case(s) failed\n",failed);return failed?1:0;
    }catch(const std::exception& error){std::printf("FAIL: %s\n",error.what());return 1;}
}
