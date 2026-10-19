#include "mikumikustudio/frame_limits.hpp"
#define NOMINMAX
#include "mikumikustudio/alembic_export.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "mikumikustudio/scene_sampling.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/channel_bake.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include <memory>
#include <algorithm>
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <fstream>
#include <map>
#include <set>
#include <cmath>
#include <stdexcept>

namespace mikumikustudio::alembic_export {
namespace {
namespace Abc=Alembic::Abc;
namespace Geom=Alembic::AbcGeom;
using Mesh=deformed_mesh_capture::Mesh;
bool exporting=false;
void WriteSample(Geom::OPolyMesh& target,const Mesh* mesh,Abc::OUInt32ArrayProperty* sourceMap=nullptr) {
    std::vector<Abc::V3f> positions,normals;
    std::vector<Abc::V2f> uvs;
    std::vector<int32_t> indices,counts;
    if(mesh) {
        for(const auto& p:mesh->positions)positions.emplace_back(p[0],p[1],p[2]);
        for(const auto& uv:mesh->uvs)uvs.emplace_back(uv[0],uv[1]);
        indices=mesh->indices;counts.assign(indices.size()/3,3);normals.assign(positions.size(),Abc::V3f(0));
        for(size_t i=0;i<indices.size();i+=3) {
            const auto a=indices[i],b=indices[i+1],c=indices[i+2];
            const auto n=(positions[b]-positions[a]).cross(positions[c]-positions[a]);
            normals[a]+=n;normals[b]+=n;normals[c]+=n;
        }
        for(auto& n:normals)if(n.length2()>0)n.normalize();
        if(mesh->normals.size()==positions.size())for(size_t i=0;i<normals.size();++i)normals[i]=Abc::V3f(mesh->normals[i][0],mesh->normals[i][1],mesh->normals[i][2]);
    }
    // Alembic treats a null array as "reuse the previous sample". Use valid
    // pointers with zero length when a material disappears at this time.
    const Abc::V3f empty3(0);const Abc::V2f empty2(0);const int32_t emptyIndex=0;
    Geom::OPolyMeshSchema::Sample sample{
        Abc::P3fArraySample(positions.empty()?&empty3:positions.data(),positions.size()),
        Abc::Int32ArraySample(indices.empty()?&emptyIndex:indices.data(),indices.size()),
        Abc::Int32ArraySample(counts.empty()?&emptyIndex:counts.data(),counts.size())};
    sample.setUVs(Geom::OV2fGeomParam::Sample(Abc::V2fArraySample(uvs.empty()?&empty2:uvs.data(),uvs.size()),Geom::kVertexScope));
    sample.setNormals(Geom::ON3fGeomParam::Sample(Abc::N3fArraySample(normals.empty()?&empty3:normals.data(),normals.size()),Geom::kVertexScope));
    target.getSchema().set(sample);
    if(sourceMap){
        const uint32_t empty=UINT32_MAX;
        const auto& ids=mesh?mesh->sourceVertices:std::vector<uint32_t>{};
        sourceMap->set(Abc::UInt32ArraySample(ids.empty()?&empty:ids.data(),ids.size()));
    }
}
}
bool IsExporting() { return exporting; }
bool CanExport(const MMDApp* app) {
    if(!app||exporting||app->PlaybackActive()||app->state.recordingWindow)return false;
    for(int i=0;i<kModelSlotCount;++i)if(app->ModelSlot(i))return true;
    for(int i=0;i<255;++i)if(app->AccessorySlot(i))return true;
    return false;
}
Result Export(MMDApp& app,const std::filesystem::path& path,const Options& options,const Progress& progress) {
    Result result;
    if(!CanExport(&app)||!app.Renderer()||!app.Renderer()->device) {result.error="No scene is available, or playback/export is active";return result;}
    if((options.writeAbc&&path.empty())||(!options.writeAbc&&!options.bakeIntoProject)||options.firstFrame<0||options.lastFrame<options.firstFrame||options.lastFrame>frame_limits::maximum||
       options.framesPerSecond<1||options.framesPerSecond>120||(options.selectedOnly&&!app.SelectedModel())) {
        result.error="Invalid path, frame range, frame rate, or model selection";return result;
    }
    std::set<std::pair<bool,int>> chosen;
    for(const auto& choice:options.selection){
        if(choice.slot<0||choice.slot>=(choice.accessory?255:kModelSlotCount)||!chosen.emplace(choice.accessory,choice.slot).second||
           (choice.accessory?!app.AccessorySlot(choice.slot):!app.ModelSlot(choice.slot))){result.error="An output object is invalid, duplicated, or has been removed";return result;}
    }
    if(options.explicitSelection&&options.selection.empty()){result.error="No output object is selected";return result;}
    if(!options.bakeIntoProject)for(const auto& choice:options.selection)if(!choice.vertices.empty()||choice.selectedBoneVertices){result.error="Vertex-range filters require native project baking; enable Bake into project";return result;}
    // ---- MMV layer cutoff bake --------------------------------------------
    // The meshes recorded are the OUTPUT of layer x (the chain [maximum..x]
    // composed once by the runtime) and the baked objects' FX on [x..maximum]
    // are removed for good after a successful commit.  Every inexpressible
    // combination is refused here or in the sampling loop, before anything is
    // written; the same-layer order is the runtime's, and nothing is composed
    // or solved a second time on this side.
    const bool layerBake=options.mmvLayerCutoff>=0;
    const std::uint32_t cutoffLayer=layerBake?std::uint32_t(options.mmvLayerCutoff):mesh_bake_cache::kNoCutoffLayer;
    std::vector<const void*> layerObjects;
    native_mesh::LayerSampling layerSampling;
    if(layerBake){
        if(!options.bakeIntoProject){result.error="A layer bake records into the project; enable Bake into project";return result;}
        if(native_mesh::LayerBakeState()!=native_mesh::LayerBakeSupport::Available){
            result.error="MMV layer baking is no longer supported in MMS; use native mesh baking or ABC export";return result;}
        std::vector<std::uint32_t> layers;std::string layerError;
        if(!native_mesh::BakeLayerList(layers,layerError)){result.error=layerError;return result;}
        if(std::find(layers.begin(),layers.end(),cutoffLayer)==layers.end()){
            result.error="The selected MMV layer does not exist in this project";return result;}
        for(const auto& choice:options.selection)
            if(!choice.materials.empty()||!choice.vertices.empty()||choice.selectedBoneVertices){
                result.error="A layer bake replaces the whole object: material, vertex or bone-vertex filters would lose the unfiltered parts of the removed FX";return result;}
        // The full-range rule covers everything the removed FX may read: the
        // scene parameters (camera, light, gravity, accessories) through the
        // channel scene scope as well as every model's key range.  A shorter
        // range would silently drop the results outside it.
        unsigned sceneLast=unsigned(std::max(0,channel_bake::LastFrame(app,channel_bake::Scope::Scene)));
        for(int i=0;i<kModelSlotCount;++i)if(auto* p=app.ModelSlot(i))sceneLast=std::max(sceneLast,mdl::Mdl(p)->maxFrame);
        if(options.firstFrame!=0||options.lastFrame<int(sceneLast)){
            result.error="A layer bake must cover frame 0 to the last frame of the scene: results outside the range would be lost with the removed FX";return result;}
        for(const auto& choice:options.selection){
            const void* object=choice.accessory?static_cast<const void*>(app.AccessorySlot(choice.slot))
                                              :static_cast<const void*>(app.ModelSlot(choice.slot));
            if(object)layerObjects.push_back(object);}
        if(!options.explicitSelection)for(int i=0;i<kModelSlotCount;++i)if(auto* p=app.ModelSlot(i))
            layerObjects.push_back(static_cast<const void*>(p));
        if(!options.explicitSelection)for(int i=0;i<255;++i)if(auto* p=app.AccessorySlot(i))
            layerObjects.push_back(static_cast<const void*>(p));
        std::sort(layerObjects.begin(),layerObjects.end());
        layerObjects.erase(std::unique(layerObjects.begin(),layerObjects.end()),layerObjects.end());
    }
    struct Busy {
        MMDApp& app;
        Busy(MMDApp& a):app(a){exporting=true;}
        ~Busy(){exporting=false;app.state.messageSeen=1;if(app.MainWindow())InvalidateRect(app.MainWindow(),nullptr,FALSE);}
    } busy(app);
    try {
        const auto target=options.writeAbc?std::filesystem::absolute(path):std::filesystem::temp_directory_path()/L"MikuMikuStudio-mesh-bake.abc";
        const auto temporary=target.parent_path()/(target.filename().wstring()+L".partial-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove(path,e);}}cleanup{temporary};
        std::set<const void*> bakedObjects;
        std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);
        stream.exceptions(std::ios::badbit|std::ios::failbit);
        {
            Alembic::AbcCoreAbstract::MetaData metadata;metadata.set("application","MikuMikuStudio");
            Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive()(&stream,metadata));
            const auto sampling=archive.addTimeSampling(Abc::TimeSampling(1.0/options.framesPerSecond,options.firstFrame/30.0));
            auto root=archive.getTop();
            Abc::OStringProperty(root.getProperties(),"coordinateSystem").set("right-handed Y-up; Z reflected from MMD; MMD units");
            Abc::OInt32Property(root.getProperties(),"sourceFrameRate").set(30);
            Abc::OInt32Property(root.getProperties(),"sampleFrameRate").set(options.framesPerSecond);
            std::map<std::string,Geom::OPolyMesh> objects;
            std::map<std::string,Abc::OUInt32ArrayProperty> sourceMaps;
            if(options.bakeIntoProject){
                Abc::OInt32Property(root.getProperties(),"mdsBakeFirstFrame").set(options.firstFrame);
                Abc::OInt32Property(root.getProperties(),"mdsBakeLastFrame").set(options.lastFrame);
            }
            std::unique_ptr<deformed_mesh_capture::Session> capture;
            if(!options.bakeIntoProject)capture=std::make_unique<deformed_mesh_capture::Session>(app,options.selectedOnly);
            struct NativeCapture {bool enabled;explicit NativeCapture(bool value):enabled(value){if(enabled)native_mesh::SetBakeCapture(true);}~NativeCapture(){if(enabled)native_mesh::SetBakeCapture(false);}} nativeCapture(options.bakeIntoProject);
            std::string samplingError;
            // The sampling session truncates the layer chain to [maximum..x]
            // and isolates the solver history for the whole run; it also makes
            // the bake capture read the cutoff layer's OUTPUT, never a second
            // host-side composition.
            if(layerBake){
                std::string layerError;
                if(!layerSampling.Begin(cutoffLayer,layerError))throw std::runtime_error(layerError);
            }
            struct PhysicsGuard{~PhysicsGuard(){bake::SetLayerPhysicsSuppressed(false);}} physicsGuard;
            // Native physics remains active during native mesh sampling.
            bake::SetLayerPhysicsSuppressed(false);
            BakePreview preview(&app);
            const bool complete=scene_sampling::Visit(app,options.firstFrame,options.lastFrame,options.framesPerSecond,
                [&](double seconds,bool output) {
                    const int frame=int(std::floor(seconds*30+1e-6));
                    if(progress&&!progress(frame,options.lastFrame)){result.cancelled=true;return false;}
                    if(capture)capture->BeginFrame();RenderFrameScene(&app);
                    if(layerBake&&output){
                        // Per-frame expressibility: everything the mesh cache
                        // cannot record must be unchanged, and a positive
                        // cutoff additionally needs the structure, bindings,
                        // poses and morphs to match the source.  Any
                        // difference is refused here - before a single write -
                        // instead of being lost with the removed FX.
                        native_mesh::LayerOutput layerOutput,layerSource;
                        std::string queryError;
                        if(!layerSampling.Query(layerOutput,queryError,&layerSource))throw std::runtime_error(queryError);
                        for(const void* object:layerObjects){
                            const auto id=std::uint64_t(reinterpret_cast<std::uintptr_t>(object));
                            const native_mesh::LayerObject* baked=nullptr;const native_mesh::LayerObject* sourceStage=nullptr;
                            for(const auto& candidate:layerOutput.objects)if(candidate.objectId==id)baked=&candidate;
                            for(const auto& candidate:layerSource.objects)if(candidate.objectId==id)sourceStage=&candidate;
                            if(!baked||!sourceStage){
                                std::string detail="The sampled layer output does not contain an object that was baked (object id "+std::to_string(id)+", output "+std::to_string(layerOutput.objects.size())+" objects:";
                                for(const auto& candidate:layerOutput.objects)detail+=" "+std::to_string(candidate.objectId);
                                detail+="; source "+std::to_string(layerSource.objects.size())+" objects:";
                                for(const auto& candidate:layerSource.objects)detail+=" "+std::to_string(candidate.objectId);
                                throw std::runtime_error(detail+")");
                            }
                            std::wstring detail;
                            if(bake::CheckLayerMeshCacheExpressible(*baked,*sourceStage,detail)!=bake::LayerError::None)
                                throw std::runtime_error("Layer bake refused before commit: the layer changed a field the mesh cache cannot store (material, additional UV, edge, flags or visibility)");
                            if(cutoffLayer>0&&!bake::CompareLayerStages(*baked,*sourceStage).Empty())
                                throw std::runtime_error("Layer bake refused before commit: a bake to a positive layer requires the structure, bindings, poses and morphs to match the source, and this layer changed them");
                        }
                    }
                    std::vector<Mesh> nativeMeshes;std::string captureError;
                    if(options.bakeIntoProject&&!native_mesh::ReadBakeCapture(nativeMeshes,captureError))throw std::runtime_error(captureError);
                    const auto& meshes=capture?capture->EndFrame():nativeMeshes;
                    if(capture&&!capture->Error().empty())throw std::runtime_error(capture->Error());
                    if(!output)return true;
                    std::set<std::string> written;
                    for(auto mesh:meshes) {
                        const bool model=mesh.name.rfind("model_",0)==0;
                        const auto slot=std::stoi(mesh.name.substr(model?6:10));
                        const void* object=model?static_cast<const void*>(app.ModelSlot(slot)):static_cast<const void*>(app.AccessorySlot(slot));
                        if(!object)continue;
                        if(options.selectedOnly&&object!=app.SelectedModel())continue;
                        const Selection* choice=nullptr;
                        if(options.explicitSelection){
                            for(const auto& candidate:options.selection)if(candidate.accessory!=model&&candidate.slot==slot){choice=&candidate;break;}
                            if(!choice)continue;
                        }
                        if(choice){
                            const auto material=uint32_t(std::stoul(mesh.name.substr(mesh.name.find("_material_")+10)));
                            if(!choice->materials.empty()&&std::find(choice->materials.begin(),choice->materials.end(),material)==choice->materials.end())continue;
                            if(options.bakeIntoProject&&(!choice->materials.empty()||!choice->vertices.empty()||choice->selectedBoneVertices)){
                                mesh.vertexPatch=true;
                                if(mesh.sourceVertices.size()!=mesh.positions.size())throw std::runtime_error("This mesh has no source-vertex mapping for a partial bake");
                                bool any=false;
                                for(auto& vertex:mesh.sourceVertices){
                                    if(vertex==UINT32_MAX)throw std::runtime_error("Generated vertices cannot be partially baked without a source index; select the complete object");
                                    bool selected=choice->vertices.empty()||std::find(choice->vertices.begin(),choice->vertices.end(),vertex)!=choice->vertices.end();
                                    if(choice->selectedBoneVertices){
                                        if(!model)throw std::runtime_error("Accessories do not have bone-weight selections");
                                        const auto& m=*mdl::Mdl(app.ModelSlot(slot));if(vertex>=m.vertexCount)throw std::runtime_error("Source vertex index exceeds model geometry");
                                        bool weighted=false;
                                        if(m.boneSelection){
                                            if(m.physicsMode==2){const auto& v=m.pmxVertices[vertex];for(int b=0;b<4;++b)if(v.bone[b]>=0&&unsigned(v.bone[b])<m.boneCount&&v.weight[b]>0&&m.boneSelection[v.bone[b]])weighted=true;}
                                            else {const auto& v=m.rawVertices[vertex];for(int b=0;b<2;++b)if(v.bone[b]>=0&&static_cast<std::uint32_t>(v.bone[b])<m.boneCount&&(b?v.weightPercent<100:v.weightPercent>0)&&m.boneSelection[v.bone[b]])weighted=true;}
                                        }
                                        selected&=weighted;
                                    }
                                    if(!selected)vertex=UINT32_MAX;else any=true;
                                }
                                if(!any)continue;
                            }
                        }
                        bakedObjects.insert(object);
                        auto found=objects.find(mesh.name);
                        if(found==objects.end()) {
                            Geom::OPolyMesh shape(root,mesh.name,sampling);
                            if(mesh.vertexPatch)
                                Abc::OBoolProperty(shape.getSchema().getUserProperties(),"mdsVertexPatch").set(true);
                            // Complete native bakes also need the original rows:
                            // capture reorders and duplicates vertices by material.
                            // Generated vertices keep their no-source marker.
                            if(options.bakeIntoProject){
                                if(mesh.sourceVertices.size()!=mesh.positions.size())
                                    throw std::runtime_error("This mesh has no source-vertex mapping for a project bake");
                                sourceMaps.emplace(mesh.name,Abc::OUInt32ArrayProperty(shape.getSchema().getUserProperties(),"mdsSourceVertices",sampling));
                            }
                            for(unsigned i=0;i<result.samples;++i)WriteSample(shape,nullptr,sourceMaps.count(mesh.name)?&sourceMaps.at(mesh.name):nullptr);
                            found=objects.emplace(mesh.name,std::move(shape)).first;
                        }
                        WriteSample(found->second,&mesh,sourceMaps.count(mesh.name)?&sourceMaps.at(mesh.name):nullptr);written.insert(mesh.name);
                    }
                    for(auto& object:objects)if(!written.count(object.first))WriteSample(object.second,nullptr,sourceMaps.count(object.first)?&sourceMaps.at(object.first):nullptr);
                    ++result.samples;
                    if(options.preview&&preview.Due(frame==options.lastFrame))preview.Present();
                    return true;
                },samplingError);
            if(!complete&&!result.cancelled)throw std::runtime_error(samplingError.empty()?"Geometry sampling did not complete":samplingError);
            result.meshes=unsigned(objects.size());
            if(!result.cancelled&&!result.meshes)throw std::runtime_error("No visible mesh draws were captured; the current rendering path cannot be exported");
        }
        stream.flush();stream.close();
        if(result.cancelled)return result;
        if(options.writeAbc){
            if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace the destination ABC file; check its permissions and whether it is open");
            result.abcWritten=true;
        }
        if(options.bakeIntoProject){
            // Validate and allocate the cache before changing effects. A failed
            // project bake leaves a successfully exported ABC available.
            // A selected controller can supply a dependency without drawing any
            // geometry. It still receives enable keys when its output is baked.
            if(options.explicitSelection)for(const auto& choice:options.selection){
                const void* object=choice.accessory?static_cast<const void*>(app.AccessorySlot(choice.slot)):static_cast<const void*>(app.ModelSlot(choice.slot));
                if(object&&(!options.selectedOnly||object==app.SelectedModel()))bakedObjects.insert(object);
            }
            const std::vector<const void*> objects(bakedObjects.begin(),bakedObjects.end());
            if(layerBake){
                // Conservative cross-object rule: with the FX of [x..maximum]
                // removed, an object outside this bake may read the baked one's
                // intermediate state through its own upper-layer FX, which no
                // cache in this build can represent.  Refused before any write.
                const auto covered=[&](const void* object){return std::find(objects.begin(),objects.end(),object)!=objects.end();};
                for(int i=0;i<kModelSlotCount;++i)if(auto* p=app.ModelSlot(i))if(!covered(static_cast<const void*>(p))){
                    result.error="A layer bake must cover every scene object: an unbaked object's upper-layer FX may read this object's intermediate state";return result;}
                for(int i=0;i<255;++i)if(auto* p=app.AccessorySlot(i))if(!covered(static_cast<const void*>(p))){
                    result.error="A layer bake must cover every scene object: an unbaked object's upper-layer FX may read this object's intermediate state";return result;}
            }
            std::string message;
            std::vector<std::uint64_t> objectIds;for(const void* object:objects)objectIds.push_back(std::uint64_t(reinterpret_cast<std::uintptr_t>(object)));
            if(!mesh_bake_cache::Install(app,options.writeAbc?target:temporary,message,
                [&](std::string& error){
                    if(!layerBake)return native_mesh::KeyBakedEffects(&app,objects,options.firstFrame,options.lastFrame,error);
                    // One transaction, in this order: open, stage the removal
                    // of the baked objects' FX on [x..maximum], commit.  The
                    // commit is the single irreversible step and everything
                    // that can fail runs before it; a failure anywhere cancels
                    // the transaction and aborts the install, so both the
                    // cache and the FX assignments stay exactly as they were.
                    native_mesh::LayerBakeTransaction transaction;
                    std::string step;
                    if(!transaction.Begin(step)||!transaction.StageRemoval(objectIds,cutoffLayer,step)||!transaction.Commit(step)){
                        error=step.empty()?std::string("The MMV layer removal could not be committed"):step;
                        return false;
                    }
                    return true;
                },cutoffLayer))
                throw std::runtime_error(message);
            result.baked=true;app.SceneModified()=1;
        }
        result.success=true;
    } catch(const std::exception& e) {result.error=e.what();}
      catch(...) {result.error="Unexpected geometry export failure";}
    // The next normal repaint rebuilds the original GPU buffers and MMV pose.
    app.state.messageSeen=1;if(app.MainWindow())InvalidateRect(app.MainWindow(),nullptr,FALSE);
    return result;
}
}
