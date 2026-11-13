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
        reference.getReferenceCapabilities=Caps17;reference.createReferenceSceneSource=Create;
        reference.readMaterialReferences=Read17;reference.readToonTable=Table17;
    }
    MmsHostV1 ReferenceHost(){auto host=Host();host.queryService=Query17;return host;}
    static ReferenceFake& Self17(void* context){return *static_cast<ReferenceFake*>(context);}
    static MmsResult MMS_CALL Query17(void* context,MmsString name,uint32_t major,uint32_t minor,uint32_t bytes,void* out){auto& f=Self17(context);
        f.queryMajor=major;f.queryMinor=minor;f.queryBytes=bytes;f.queriedName=name.bytes==18&&!std::memcmp(name.data,"mms.scene.evaluate",18);
        if(!out||bytes<sizeof(f.reference))return MMS_E_ARGUMENT;std::memcpy(out,&f.reference,sizeof(f.reference));return f.queryResult;
    }
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
void References(){
    stage="reference sdk";ReferenceFake fake;sdk::EvaluationReferenceServices service;
    CHECK(sdk::EvaluationReferenceServices::Query(fake.ReferenceHost(),service)==MMS_OK&&service.ready());
    CHECK(fake.queryMajor==1&&fake.queryMinor==7&&fake.queryBytes==sizeof(MmsEvaluationApiV1_7)&&fake.queriedName);
    MmsEvaluationReferenceCapsV1 caps{};CHECK(service.Capabilities(caps)==MMS_OK&&caps.flags==MMS_EVALUATION_REFERENCE_CAP_RUNTIME_FIELDS);
    auto badCaps=Poison<MmsEvaluationReferenceCapsV1>();fake.fault=1;CHECK(service.Capabilities(badCaps)==MMS_E_FAILED&&Unchanged(badCaps));fake.fault=0;
    sdk::EvaluationReferenceSource source;CHECK(service.Capture(source,1234)==MMS_OK&&source.get());
    CHECK(source.Material().get()==source.get()&&source.Material().Mesh().get()==source.get()&&fake.captureQuery.maxRetainedBytes==1234);
    CHECK(service.Capture(source)==MMS_E_BUSY);
    MmsEvaluationMaterialReferencesV1 refs{};CHECK(source.ReadMaterialReferences(0,1,refs)==MMS_OK&&refs.index==1&&refs.texturePath[0]==0x9ad8);
    MmsEvaluationToonTableV1 table{};CHECK(source.ReadToonTable(0,table)==MMS_OK&&table.fileNames[9][99]==0x81);
    auto poisonRefs=Poison<MmsEvaluationMaterialReferencesV1>();
    for(int fault:{2,3,4,5,6,7,8,9}){fake.fault=fault;CHECK(source.ReadMaterialReferences(0,1,poisonRefs)==(fault==2?MMS_E_VERSION:MMS_E_FAILED)&&Unchanged(poisonRefs));}
    auto poisonTable=Poison<MmsEvaluationToonTableV1>();
    for(int fault:{10,11}){fake.fault=fault;CHECK(source.ReadToonTable(0,poisonTable)==(fault==10?MMS_E_VERSION:MMS_E_FAILED)&&Unchanged(poisonTable));}
    fake.fault=0;fake.referenceResult=MMS_E_CANCELLED;CHECK(source.ReadMaterialReferences(0,1,poisonRefs)==MMS_E_CANCELLED&&Unchanged(poisonRefs));fake.referenceResult=MMS_OK;
    sdk::EvaluationReferenceSource retained;CHECK(source.Retain(retained)==MMS_OK&&retained.get()!=source.get());
    CHECK(source.Retain(retained)==MMS_E_BUSY);
    const auto original=source.get();fake.results[Release]=MMS_E_BUSY;CHECK(source.Release()==MMS_E_BUSY&&source.get()==original);
    fake.results[Release]=MMS_OK;CHECK(source.Release()==MMS_OK&&!source);
    CHECK(retained.ReadToonTable(0,table)==MMS_OK&&retained.Release()==MMS_OK&&fake.Live()==0);
    sdk::EvaluationReferenceServices preserved=service;fake.queryResult=MMS_E_VERSION;
    CHECK(sdk::EvaluationReferenceServices::Query(fake.ReferenceHost(),preserved)==MMS_E_VERSION&&preserved.ready());
    Fake old;sdk::EvaluationReferenceServices absent;CHECK(sdk::EvaluationReferenceServices::Query(old.Host(),absent)==MMS_E_VERSION&&!absent.ready());
}
}
int main(){try{References();}catch(...){std::fprintf(stderr,"exception in %s\n",stage);return 1;}
    std::printf("evaluation_reference_cpp_tests: %u checks passed\n",checks);return 0;}
