// Reuse the real 1.4-1.6 fake tables, then add a 1.7 source-only table.
#include <algorithm>
#define main existing_evaluation_sdk_main
#include "evaluation_cpp_sdk_tests.cpp"
#undef main

namespace {
struct ReferenceFake : MaterialFake {
    MmsEvaluationApiV1_7 reference{};
    unsigned referenceCalls=0,tableCalls=0;
    int fault=0;
    MmsResult referenceResult=MMS_OK;
    ReferenceFake() {
        std::memcpy(&reference,&material,sizeof(material));reference.h={sizeof(reference),1,7};
        reference.getReferenceCapabilities=Caps17;reference.createReferenceSceneSource=Create17;
        reference.readMaterialReferences=Read17;reference.readToonTable=Table17;
    }
    MmsHostV1 ReferenceHost(){auto host=Host();host.queryService=Query17;return host;}
    static ReferenceFake& Self17(void* context){return *static_cast<ReferenceFake*>(context);}
    static MmsResult MMS_CALL Query17(void* context,MmsString name,uint32_t major,uint32_t minor,uint32_t bytes,void* out){auto& f=Self17(context);
        f.queryMajor=major;f.queryMinor=minor;f.queryBytes=bytes;f.queriedName=name.bytes==18&&!std::memcmp(name.data,"mms.scene.evaluate",18);
        if(!out||bytes<sizeof(f.reference))return MMS_E_ARGUMENT;std::memcpy(out,&f.reference,sizeof(f.reference));return f.queryResult;
    }
    static MmsResult MMS_CALL Create17(void* context,const MmsEvaluationSceneQueryV1* query,MmsHandle* out){return Create(context,query,out);}
    static MmsResult MMS_CALL Caps17(void* context,MmsEvaluationReferenceCapsV1* out){auto& f=Self17(context);
        *out={H<MmsEvaluationReferenceCapsV1>(),MMS_EVALUATION_REFERENCE_CAP_RUNTIME_FIELDS,0};if(f.fault==1)out->reserved=1;return f.referenceResult;
    }
    static MmsResult MMS_CALL Read17(void* context,MmsHandle handle,uint32_t model,uint32_t index,MmsEvaluationMaterialReferencesV1* out){auto& f=Self17(context);++f.referenceCalls;
        if(!Range(f,handle,model,index,1,2,false))return MMS_E_NOT_FOUND;
        *out={};out->h=H<MmsEvaluationMaterialReferencesV1>();out->material={100+model,200+index};out->index=index;
        out->flags=model?MMS_EVALUATION_MATERIAL_FORMAT_PMD:MMS_EVALUATION_MATERIAL_FORMAT_PMX;
        out->texturePath[0]=0x9ad8;out->spherePath[0]='s';out->toonPath[0]='t';out->toonReference=0xfe;
        if(f.fault==2)out->h.major=2;if(f.fault==3)out->material={};if(f.fault==4)out->index++;
        if(f.fault==5)out->flags=0;if(f.fault==6)out->reserved=1;
        if(f.fault==7)std::fill(std::begin(out->texturePath),std::end(out->texturePath),uint16_t('x'));
        if(f.fault==8)std::fill(std::begin(out->spherePath),std::end(out->spherePath),uint16_t('x'));
        if(f.fault==9)std::fill(std::begin(out->toonPath),std::end(out->toonPath),uint16_t('x'));
        return f.referenceResult;
    }
    static MmsResult MMS_CALL Table17(void* context,MmsHandle handle,uint32_t model,MmsEvaluationToonTableV1* out){auto& f=Self17(context);++f.tableCalls;
        if(!f.Find(handle)||model>1)return MMS_E_NOT_FOUND;
        *out={};out->h=H<MmsEvaluationToonTableV1>();out->modelDirectory[0]='D';std::memset(out->fileNames[9],0x81,100);
        if(f.fault==10)out->h.major=2;if(f.fault==11)std::fill(std::begin(out->modelDirectory),std::end(out->modelDirectory),uint16_t('x'));
        return f.referenceResult;
    }
};
static_assert(!std::is_copy_constructible_v<sdk::EvaluationReferenceSource>);
static_assert(!std::is_move_assignable_v<sdk::EvaluationReferenceSource>);
static_assert(std::is_nothrow_move_constructible_v<sdk::EvaluationReferenceSource>);

template<class Api,class HostApi>
void CheckApi(const Api& api,uint16_t minor,const HostApi& hostApi){
    CHECK(api.h.size==sizeof(api));
    CHECK(api.h.major==1&&api.h.minor==minor);
    CHECK(api.context==hostApi.context&&api.retain==hostApi.retain&&api.release==hostApi.release);
}

template<class Material,class HostApi>
void CheckMaterialViews(const Material& material,const HostApi& hostApi){
    const auto& mesh=material.Mesh();
    CheckApi(material.api(),6,hostApi);
    CheckApi(mesh.api(),5,hostApi);
    CHECK(material.get()&&mesh.get()==material.get());
    CHECK(material.api().materialAt==hostApi.materialAt&&material.api().readFrameMaterials==hostApi.readFrameMaterials);
    CHECK(mesh.api().readBonePose==hostApi.readBonePose&&mesh.api().readMeshVertices==hostApi.readMeshVertices);
}

void PrefixViews(){
    stage="reference prefix views";ReferenceFake fake;
    const auto referenceTable=fake.reference;
    sdk::EvaluationReferenceServices referenceService;
    CHECK(sdk::EvaluationReferenceServices::Query(fake.ReferenceHost(),referenceService)==MMS_OK);
    sdk::EvaluationReferenceSource source,retained;
    CHECK(referenceService.Capture(source)==MMS_OK);
    CheckApi(source.api(),7,referenceTable);
    CheckMaterialViews(source.Material(),referenceTable);
    CHECK(source.get()==source.Material().get());
    CHECK(source.Retain(retained)==MMS_OK&&retained.get()!=source.get());
    sdk::EvaluationReferenceSource moved(std::move(retained));
    CHECK(!retained&&!retained.get());
    CheckApi(moved.api(),7,referenceTable);
    CheckMaterialViews(moved.Material(),referenceTable);
    CHECK(moved.get()==moved.Material().get());
    sdk::EvaluationMaterialFrame frame,retainedFrame;
    CHECK(moved.Material().Evaluate({1,2},frame)==MMS_OK);
    CheckMaterialViews(frame,referenceTable);
    CHECK(frame.Retain(retainedFrame)==MMS_OK&&retainedFrame.get()!=frame.get());
    CheckMaterialViews(retainedFrame,referenceTable);
    CHECK(source.Release()==MMS_OK&&moved.Release()==MMS_OK&&frame.Release()==MMS_OK);
    std::vector<MmsEvaluationMaterialV1> materials;
    std::vector<MmsEvaluationMeshVertexV1> vertices;
    CHECK(retainedFrame.ReadMaterials(0,1,1,materials)==MMS_OK&&materials.size()==1&&materials[0].index==1);
    CHECK(retainedFrame.Mesh().ReadVertices(0,1,1,vertices)==MMS_OK&&vertices.size()==1&&vertices[0].position[0]==11.f);
    CHECK(retainedFrame.Release()==MMS_OK&&fake.Live()==0);
    CheckApi(referenceService.api(),7,referenceTable);
    CHECK(Same(referenceService.api().h,referenceTable.h)&&Same(fake.reference.h,referenceTable.h));

    stage="material prefix views";
    const auto materialTable=fake.material;
    sdk::EvaluationMaterialServices materialService;sdk::EvaluationMaterialSource materialSource;
    CHECK(sdk::EvaluationMaterialServices::Query(fake.MaterialHost(),materialService)==MMS_OK);
    CHECK(materialService.Capture(materialSource)==MMS_OK);
    CheckMaterialViews(materialSource,materialTable);
    CHECK(materialSource.Evaluate({1,2},frame)==MMS_OK);
    CheckMaterialViews(frame,materialTable);
    const auto detachedApi=frame.Mesh().api();
    const auto handle=frame.detach();
    CHECK(handle&&!frame&&detachedApi.release(detachedApi.context,handle)==MMS_OK);
    CHECK(materialSource.Release()==MMS_OK&&fake.Live()==0);
    CheckApi(materialService.api(),6,materialTable);
    CHECK(Same(materialService.api().h,materialTable.h)&&Same(fake.material.h,materialTable.h));
}

template<class Service,class Source,class Api>
void CheckCompatibleQuery(const MmsHostV1& host,Api& hostApi,uint16_t minor){
    // This fake writes only the supplied prefix but advertises a newer table.
    // Keep compatible negotiation, without publishing bytes we never received.
    const auto original=hostApi.h;
    const MmsHeader advertised{static_cast<uint32_t>(sizeof(Api)+sizeof(void*)),1,static_cast<uint16_t>(minor+1)};
    hostApi.h=advertised;
    Service service;Source source;
    CHECK(Service::Query(host,service)==MMS_OK&&service.ready());
    CheckApi(service.api(),minor,hostApi);
    CHECK(service.Capture(source)==MMS_OK);
    CheckApi(source.api(),minor,hostApi);
    CHECK(source.Release()==MMS_OK&&Same(hostApi.h,advertised));
    hostApi.h=original;
}

void CompatibleQueries(){
    stage="compatible queries";ReferenceFake fake;
    CheckCompatibleQuery<sdk::EvaluationServices,sdk::EvaluationSceneSource>(fake.Host(),fake.table,4);
    CheckCompatibleQuery<sdk::EvaluationMeshServices,sdk::EvaluationMeshSource>(fake.MeshHost(),fake.mesh,5);
    CheckCompatibleQuery<sdk::EvaluationMaterialServices,sdk::EvaluationMaterialSource>(fake.MaterialHost(),fake.material,6);
    CheckCompatibleQuery<sdk::EvaluationReferenceServices,sdk::EvaluationReferenceSource>(fake.ReferenceHost(),fake.reference,7);
    CHECK(fake.Live()==0);
}

void InternalLeasePrefix(){
    // Mesh wrappers store a separate 1.4 lease table without exposing it via api().
    stage="internal lease prefix";MeshFake fake;
    CheckApi(sdk::evaluation_detail::Prefix(fake.mesh),4,fake.mesh);
}

void References(){
    stage="reference sdk";ReferenceFake fake;sdk::EvaluationReferenceServices service;
    CHECK(sdk::EvaluationReferenceServices::Query(fake.ReferenceHost(),service)==MMS_OK&&service.ready());
    CHECK(fake.queryMajor==1&&fake.queryMinor==7&&fake.queryBytes==sizeof(MmsEvaluationApiV1_7)&&fake.queriedName);
    MmsEvaluationReferenceCapsV1 caps{};CHECK(service.Capabilities(caps)==MMS_OK&&caps.flags==MMS_EVALUATION_REFERENCE_CAP_RUNTIME_FIELDS);
    auto badCaps=Poison<MmsEvaluationReferenceCapsV1>();fake.fault=1;CHECK(service.Capabilities(badCaps)==MMS_E_FAILED&&Unchanged(badCaps));fake.fault=0;
    sdk::EvaluationReferenceSource source;CHECK(service.Capture(source,1234)==MMS_OK&&source.get());
    CHECK(source.Material().get()==source.get()&&source.Material().Mesh().get()==source.get()&&fake.captureQuery.maxRetainedBytes==1234);
    // Borrowed C tables describe their own prefix, with the same callbacks and
    // context as a direct C query for that version and the same source handle.
    CheckApiTable(source.api(),fake.reference);
    CheckApiTable(source.Material().api(),fake.material);
    CheckApiTable(source.Material().Mesh().api(),fake.mesh);
    MmsEvaluationMaterialIdV1 id{};
    const auto& material=source.Material().api();
    CHECK(material.materialAt(material.context,source.get(),0,1,&id)==MMS_OK&&id.material.hi==201);
    sdk::EvaluationMaterialFrame frame;
    CHECK(source.Material().Evaluate({1,2},frame)==MMS_OK&&frame.Mesh().get()==frame.get());
    CheckApiTable(frame.api(),fake.material);
    CheckApiTable(frame.Mesh().api(),fake.mesh);
    std::vector<MmsEvaluationMaterialV1> values;
    CHECK(frame.ReadMaterials(0,1,1,values)==MMS_OK&&values.size()==1&&values[0].material.hi==201);
    CHECK(frame.Release()==MMS_OK);
    CHECK(service.Capture(source)==MMS_E_BUSY);
    MmsEvaluationMaterialReferencesV1 refs{};CHECK(source.ReadMaterialReferences(0,1,refs)==MMS_OK&&refs.index==1&&refs.texturePath[0]==0x9ad8);
    MmsEvaluationToonTableV1 table{};CHECK(source.ReadToonTable(0,table)==MMS_OK&&table.fileNames[9][99]==0x81);
    auto poisonRefs=Poison<MmsEvaluationMaterialReferencesV1>();
    for(int fault:{2,3,4,5,6,7,8,9}){fake.fault=fault;CHECK(source.ReadMaterialReferences(0,1,poisonRefs)==(fault==2?MMS_E_VERSION:MMS_E_FAILED)&&Unchanged(poisonRefs));}
    auto poisonTable=Poison<MmsEvaluationToonTableV1>();
    for(int fault:{10,11}){fake.fault=fault;CHECK(source.ReadToonTable(0,poisonTable)==(fault==10?MMS_E_VERSION:MMS_E_FAILED)&&Unchanged(poisonTable));}
    fake.fault=0;fake.referenceResult=MMS_E_CANCELLED;CHECK(source.ReadMaterialReferences(0,1,poisonRefs)==MMS_E_CANCELLED&&Unchanged(poisonRefs));fake.referenceResult=MMS_OK;
    sdk::EvaluationReferenceSource retained;CHECK(source.Retain(retained)==MMS_OK&&retained.get()!=source.get());
    CheckApiTable(retained.Material().api(),fake.material);
    CheckApiTable(retained.Material().Mesh().api(),fake.mesh);
    CHECK(source.Retain(retained)==MMS_E_BUSY);
    const auto original=source.get();fake.results[Release]=MMS_E_BUSY;CHECK(source.Release()==MMS_E_BUSY&&source.get()==original);
    fake.results[Release]=MMS_OK;CHECK(source.Release()==MMS_OK&&!source);
    CHECK(retained.ReadToonTable(0,table)==MMS_OK&&retained.Release()==MMS_OK&&fake.Live()==0);
    sdk::EvaluationReferenceServices preserved=service;fake.queryResult=MMS_E_VERSION;
    CHECK(sdk::EvaluationReferenceServices::Query(fake.ReferenceHost(),preserved)==MMS_E_VERSION&&preserved.ready());
    Fake old;sdk::EvaluationReferenceServices absent;CHECK(sdk::EvaluationReferenceServices::Query(old.Host(),absent)==MMS_E_VERSION&&!absent.ready());
}
}
int main(){try{References();PrefixViews();CompatibleQueries();InternalLeasePrefix();}catch(...){std::fprintf(stderr,"exception in %s\n",stage);return 1;}
    std::printf("evaluation_reference_cpp_tests: %u checks passed\n",checks);return 0;}
