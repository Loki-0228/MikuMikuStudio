#include <mms/evaluation.h>
#include <mms/cpp/evaluation.hpp>
#include <cstring>
#pragma comment(linker, "/EXPORT:MMSPluginGetApi")
namespace {
MmsEvaluationApiV1 api{};
MmsEvaluationApiV1_1 api11{};
MmsEvaluationApiV1_2 api12{};
MmsEvaluationApiV1_3 api13{};
MmsEvaluationApiV1_4 api14{};
MmsEvaluationApiV1_5 api15{};
MmsEvaluationApiV1_6 api16{};
mms::sdk::EvaluationServices sdkServices;
mms::sdk::EvaluationMeshServices sdkMeshServices;
mms::sdk::EvaluationMaterialServices sdkMaterialServices;
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1*, void** out) {
    if (!host || !out) return MMS_E_ARGUMENT;
    constexpr char service[] = "mms.scene.evaluate";
    const auto status = host->queryService(host->context, {service, sizeof(service)-1}, 1, 0, sizeof(api), &api);
    if (status != MMS_OK) return status;
    const auto extended = host->queryService(host->context, {service, sizeof(service)-1}, 1, 1, sizeof(api11), &api11);
    if (extended != MMS_OK) return extended;
    const auto display = host->queryService(host->context, {service, sizeof(service)-1}, 1, 2, sizeof(api12), &api12);
    if (display != MMS_OK) return display;
    const auto parent = host->queryService(host->context, {service, sizeof(service)-1}, 1, 3, sizeof(api13), &api13);
    if (parent != MMS_OK) return parent;
    const auto scene = host->queryService(host->context, {service, sizeof(service)-1}, 1, 4, sizeof(api14), &api14);
    if (scene != MMS_OK) return scene;
    const auto mesh = host->queryService(host->context, {service, sizeof(service)-1}, 1, 5, sizeof(api15), &api15);
    if (mesh != MMS_OK) return mesh;
    // Optional for the older host checks; the 1.6 host fixture requires it
    // explicitly in its manifest and verifies the returned table below.
    if (host->queryService(host->context, {service, sizeof(service)-1}, 1, 6, sizeof(api16), &api16) != MMS_OK) api16 = {};
    const auto sdk = mms::sdk::EvaluationServices::Query(*host, sdkServices);
    if (sdk != MMS_OK) return sdk;
    const auto meshSdk = mms::sdk::EvaluationMeshServices::Query(*host, sdkMeshServices);
    if (meshSdk != MMS_OK) return meshSdk;
    if (api16.context) {
        const auto materialSdk = mms::sdk::EvaluationMaterialServices::Query(*host, sdkMaterialServices);
        if (materialSdk != MMS_OK) return materialSdk;
    }
    if (sdk == MMS_OK) *out = &api;
    return sdk;
}
MmsResult MMS_CALL Start(void*) { return MMS_OK; }
void MMS_CALL Event(void*, const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void*) { return MMS_OK; }
void MMS_CALL Destroy(void*) { api = {}; api11 = {}; api12 = {}; api13 = {}; api14 = {}; api15 = {}; api16 = {}; sdkServices = {}; sdkMeshServices = {}; sdkMaterialServices = {}; }
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi(MmsEvaluationApiV1* out) {
    if (!out || !api.context) return MMS_E_NOT_READY;
    *out = api; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi11(MmsEvaluationApiV1_1* out) {
    if (!out || !api11.context) return MMS_E_NOT_READY;
    *out = api11; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi12(MmsEvaluationApiV1_2* out) {
    if (!out || !api12.context) return MMS_E_NOT_READY;
    *out = api12; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi13(MmsEvaluationApiV1_3* out) {
    if (!out || !api13.context) return MMS_E_NOT_READY;
    *out = api13; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi14(MmsEvaluationApiV1_4* out) {
    if (!out || !api14.context) return MMS_E_NOT_READY;
    *out = api14; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi15(MmsEvaluationApiV1_5* out) {
    if (!out || !api15.context) return MMS_E_NOT_READY;
    *out = api15; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi16(MmsEvaluationApiV1_6* out) {
    if (!out || !api16.context) return MMS_E_NOT_READY;
    *out = api16; return MMS_OK;
}
// Called by the host fixture only. The plugin owns the wrappers and releases
// the source before reading its frame, proving the SDK uses independent leases.
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostSdkMaterials(
    MmsEvaluationMaterialIdV1* ids, MmsEvaluationMaterialV1* values, uint32_t count) {
    if(!ids || !values || count!=2)return MMS_E_ARGUMENT;
    mms::sdk::EvaluationMaterialSource source;
    mms::sdk::EvaluationMaterialFrame frame;
    MmsEvaluationMaterialIdV1 copiedIds[2]{};
    std::vector<MmsEvaluationMaterialV1> copiedValues;
    auto result=sdkMaterialServices.Capture(source);
    if(result==MMS_OK)result=source.MaterialAt(0,0,copiedIds[0]);
    if(result==MMS_OK)result=source.MaterialAt(0,1,copiedIds[1]);
    if(result==MMS_OK)result=source.Evaluate({0,1},frame);
    if(result==MMS_OK)result=source.Release();
    if(result==MMS_OK)result=frame.ReadMaterials(0,0,count,copiedValues);
    if(result==MMS_OK && copiedValues.size()!=count)result=MMS_E_FAILED;
    if(result==MMS_OK)result=frame.Release();
    if(result==MMS_OK){std::memcpy(ids,copiedIds,sizeof(copiedIds));std::memcpy(values,copiedValues.data(),count*sizeof(*values));}
    return result;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostSdkMesh(
    MmsEvaluationMeshInfoV1* info, uint32_t* index, MmsEvaluationMeshMaterialV1* material,
    MmsEvaluationMeshVertexV1* bindVertex, MmsEvaluationMeshVertexV1* frameVertex,
    MmsEvaluationSceneModelV1* model) {
    if(!info||!index||!material||!bindVertex||!frameVertex||!model)return MMS_E_ARGUMENT;
    mms::sdk::EvaluationMeshSource source, alias;
    mms::sdk::EvaluationMeshFrame frame, frameAlias;
    MmsEvaluationMeshInfoV1 infoCopy{};MmsEvaluationSceneModelV1 modelCopy{};
    std::vector<uint32_t> indices;std::vector<MmsEvaluationMeshMaterialV1> materials;
    std::vector<MmsEvaluationMeshVertexV1> binding, vertices;
    auto result=sdkMeshServices.Capture(source);
    if(result==MMS_OK)result=source.Retain(alias);
    if(result==MMS_OK)result=source.Release();
    if(result==MMS_OK)result=alias.MeshInfo(0,infoCopy);
    if(result==MMS_OK&&(!infoCopy.vertexCount||!infoCopy.indexCount||!infoCopy.materialCount))result=MMS_E_NOT_FOUND;
    if(result==MMS_OK)result=alias.ReadIndices(0,0,1,indices);
    if(result==MMS_OK)result=alias.ReadMaterials(0,0,1,materials);
    if(result==MMS_OK)result=alias.ReadBindVertices(0,0,1,binding);
    if(result==MMS_OK)result=alias.Evaluate({1,3},frame);
    if(result==MMS_OK)result=alias.Release();
    if(result==MMS_OK)result=frame.Retain(frameAlias);
    if(result==MMS_OK)result=frame.Release();
    if(result==MMS_OK)result=frameAlias.ModelAt(0,modelCopy);
    if(result==MMS_OK)result=frameAlias.ReadVertices(0,0,1,vertices);
    if(result==MMS_OK)result=frameAlias.Release();
    if(result==MMS_OK){*info=infoCopy;*index=indices[0];*material=materials[0];*bindVertex=binding[0];*frameVertex=vertices[0];*model=modelCopy;}
    return result;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostSdkFrame(MmsEvaluationSceneModelV1* model, MmsEvaluationScenePoseV1* pose) {
    if (!model || !pose) return MMS_E_ARGUMENT;
    mms::sdk::EvaluationSceneSource source;
    mms::sdk::EvaluationSceneFrame frame;
    MmsEvaluationSceneModelV1 modelCopy{};MmsEvaluationScenePoseV1 poseCopy{};
    auto result=sdkServices.Capture(source);
    if(result==MMS_OK)result=source.Evaluate({1,3},frame);
    if(result==MMS_OK)result=source.Release();
    if(result==MMS_OK)result=frame.ModelAt(0,modelCopy);
    if(result==MMS_OK)result=frame.ReadBonePose(0,0,poseCopy);
    if(result==MMS_OK)result=frame.Release();
    if(result==MMS_OK){*model=modelCopy;*pose=poseCopy;}
    return result;
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor,
    uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor) return MMS_E_VERSION;
    *out = {{sizeof(*out),1,0},Create,Start,Event,Stop,Destroy}; return MMS_OK;
}
