#include <mms/evaluation.h>
#include <cstring>
#include <string>

namespace {
template<class T> MmsHeader H() { return {sizeof(T), 1, 0}; }
MmsString Text(const char* text) { return {text, std::strlen(text)}; }
struct State {
    MmsHostV1 host{};
    MmsEvaluationApiV1 api{};
    MmsEvaluationApiV1_1 api11{};
    MmsEvaluationApiV1_2 api12{};
    MmsEvaluationApiV1_3 api13{};
    MmsEvaluationApiV1_4 api14{};
    MmsEvaluationApiV1_5 api15{};
    MmsEvaluationApiV1_6 api16{};
    MmsEvaluationApiV1_7 api17{};
    MmsHandle source = 0;
    uint32_t stops = 0;
    bool holdOk = false;
} state;
void Log(const char* text) { state.host.log(state.host.context, 1, Text(text)); }
bool Negotiate() {
    static_assert(sizeof(MmsEvaluationApiV1) == 72 && sizeof(MmsEvaluationApiV1_1) == 104 && sizeof(MmsEvaluationApiV1_2) == 152 && sizeof(MmsEvaluationApiV1_3) == 160 && sizeof(MmsEvaluationApiV1_4) == 232);
    struct Buffer { MmsEvaluationApiV1_7 table; uint64_t guard; } buffer;
    std::memset(&buffer, 0x5a, sizeof(buffer)); const auto before = buffer;
    const auto query = [&](uint32_t major, uint32_t minor, uint32_t bytes) {
        return state.host.queryService(state.host.context, Text("mms.scene.evaluate"), major, minor, bytes, &buffer.table);
    };
    if (query(2, 0, 152) != MMS_E_VERSION || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 8, sizeof(buffer.table)) != MMS_E_VERSION || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 7, sizeof(MmsEvaluationApiV1_7)-1) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 6, sizeof(MmsEvaluationApiV1_5)) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 6, sizeof(MmsEvaluationApiV1_6)-1) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 5, 232) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 4, 160) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 4, 231) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 3, 152) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 3, 159) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 2, 104) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 2, 151) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 1, 72) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 1, 103) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 0, 71) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 0, 72, nullptr) != MMS_E_ARGUMENT) return false;
    if (state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 2, 152, nullptr) != MMS_E_ARGUMENT) return false;
    if (state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 3, 160, nullptr) != MMS_E_ARGUMENT) return false;
    if (state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 4, 232, nullptr) != MMS_E_ARGUMENT) return false;
    if (query(1, 0, 72) != MMS_OK) return false;
    if (std::memcmp(reinterpret_cast<const char*>(&buffer) + 72, reinterpret_cast<const char*>(&before) + 72, sizeof(buffer) - 72)) return false;
    MmsEvaluationApiV1 out{}; std::memcpy(&out, &buffer.table, sizeof(out));
    if (out.h.size != 72 || out.h.major != 1 || out.h.minor) return false;
    if (!out.context || !out.getCapabilities || !out.createSource || !out.describeSource || !out.boneAt || !out.sampleBone || !out.retain || !out.release) return false;
    state.api = out;
    buffer = before;
    if (query(1, 1, 104) != MMS_OK) return false;
    if (std::memcmp(reinterpret_cast<const char*>(&buffer) + 104, reinterpret_cast<const char*>(&before) + 104, sizeof(buffer) - 104)) return false;
    MmsEvaluationApiV1_1 extended{}; std::memcpy(&extended, &buffer.table, sizeof(extended));
    if (extended.h.size != 104 || extended.h.major != 1 || extended.h.minor != 1 ||
        extended.context != out.context || extended.getCapabilities != out.getCapabilities ||
        extended.createSource != out.createSource || extended.describeSource != out.describeSource ||
        extended.boneAt != out.boneAt || extended.sampleBone != out.sampleBone ||
        extended.retain != out.retain || extended.release != out.release ||
        !extended.createSourceChannels || !extended.describeSourceChannels || !extended.morphAt || !extended.sampleMorph) return false;
    state.api11 = extended;
    buffer = before;
    if (query(1, 2, 152) != MMS_OK) return false;
    if (std::memcmp(reinterpret_cast<const char*>(&buffer) + 152, reinterpret_cast<const char*>(&before) + 152, sizeof(buffer) - 152)) return false;
    MmsEvaluationApiV1_2 display{}; std::memcpy(&display, &buffer.table, sizeof(display));
    if (display.h.size != 152 || display.h.major != 1 || display.h.minor != 2 ||
        display.context != extended.context || display.getCapabilities != extended.getCapabilities ||
        display.createSource != extended.createSource || display.describeSource != extended.describeSource ||
        display.boneAt != extended.boneAt || display.sampleBone != extended.sampleBone ||
        display.retain != extended.retain || display.release != extended.release ||
        display.createSourceChannels != extended.createSourceChannels || display.describeSourceChannels != extended.describeSourceChannels ||
        display.morphAt != extended.morphAt || display.sampleMorph != extended.sampleMorph ||
        !display.describeSourceDisplay || !display.ikAt || !display.selectorAt ||
        !display.sampleVisibility || !display.sampleIk || !display.sampleParent) return false;
    state.api12 = display;
    buffer = before;
    if (query(1, 3, 160) != MMS_OK ||
        std::memcmp(reinterpret_cast<const char*>(&buffer) + 160, reinterpret_cast<const char*>(&before) + 160, sizeof(buffer) - 160)) return false;
    MmsEvaluationApiV1_3 parent{}; std::memcpy(&parent, &buffer.table, sizeof(parent));
    if (parent.h.size != 160 || parent.h.major != 1 || parent.h.minor != 3 || !parent.sampleParentState) return false;
    // The complete 1.2 context/function-pointer prefix must remain identical.
    if (std::memcmp(reinterpret_cast<const char*>(&parent) + sizeof(MmsHeader),
                    reinterpret_cast<const char*>(&display) + sizeof(MmsHeader),
                    sizeof(display) - sizeof(MmsHeader))) return false;
    state.api13 = parent;
    buffer = before;
    if (query(1, 4, 232) != MMS_OK || std::memcmp(reinterpret_cast<const char*>(&buffer) + 232, reinterpret_cast<const char*>(&before) + 232, sizeof(buffer) - 232)) return false;
    MmsEvaluationApiV1_4 scene{};std::memcpy(&scene,&buffer.table,sizeof(scene));
    if (scene.h.size != 232 || scene.h.major != 1 || scene.h.minor != 4 ||
        std::memcmp(reinterpret_cast<const char*>(&scene) + sizeof(MmsHeader),
                    reinterpret_cast<const char*>(&parent) + sizeof(MmsHeader),
                    sizeof(parent) - sizeof(MmsHeader)) || !scene.getSceneCapabilities ||
        !scene.createSceneSource || !scene.describeSceneSource || !scene.sceneModelAt ||
        !scene.sceneBoneAt || !scene.evaluateFrame || !scene.describeFrame ||
        !scene.frameModelAt || !scene.readBonePose) return false;
    state.api14 = scene;
    buffer=before;
    if(query(1,5,sizeof(MmsEvaluationApiV1_5))!=MMS_OK||
        std::memcmp(reinterpret_cast<const char*>(&buffer)+sizeof(MmsEvaluationApiV1_5),reinterpret_cast<const char*>(&before)+sizeof(MmsEvaluationApiV1_5),sizeof(buffer)-sizeof(MmsEvaluationApiV1_5)))return false;
    MmsEvaluationApiV1_5 mesh{};std::memcpy(&mesh,&buffer.table,sizeof(mesh));
    if(mesh.h.size!=sizeof(mesh)||mesh.h.major!=1||mesh.h.minor!=5||
        std::memcmp(reinterpret_cast<const char*>(&mesh)+sizeof(MmsHeader),reinterpret_cast<const char*>(&scene)+sizeof(MmsHeader),sizeof(scene)-sizeof(MmsHeader))||
        !mesh.getMeshCapabilities||!mesh.createMeshSceneSource||!mesh.describeMesh||!mesh.readMeshIndices||!mesh.readMeshMaterials||!mesh.readMeshBindVertices||!mesh.readMeshVertices)return false;
    state.api15=mesh;
    buffer=before;
    if(query(1,6,sizeof(MmsEvaluationApiV1_6))!=MMS_OK||std::memcmp(reinterpret_cast<const char*>(&buffer)+sizeof(MmsEvaluationApiV1_6),reinterpret_cast<const char*>(&before)+sizeof(MmsEvaluationApiV1_6),sizeof(buffer)-sizeof(MmsEvaluationApiV1_6)))return false;
    MmsEvaluationApiV1_6 material{};std::memcpy(&material,&buffer.table,sizeof(material));
    if(material.h.size!=sizeof(material)||material.h.major!=1||material.h.minor!=6||
        std::memcmp(reinterpret_cast<const char*>(&material)+sizeof(MmsHeader),reinterpret_cast<const char*>(&mesh)+sizeof(MmsHeader),sizeof(mesh)-sizeof(MmsHeader))||
        !material.getMaterialCapabilities||!material.createMaterialSceneSource||!material.materialAt||!material.readFrameMaterials)return false;
    state.api16=material;
    buffer=before;
    if(query(1,7,sizeof(MmsEvaluationApiV1_7))!=MMS_OK||buffer.guard!=before.guard)return false;
    const auto reference=buffer.table;
    if(reference.h.size!=sizeof(reference)||reference.h.major!=1||reference.h.minor!=7||
        std::memcmp(reinterpret_cast<const char*>(&reference)+sizeof(MmsHeader),reinterpret_cast<const char*>(&material)+sizeof(MmsHeader),sizeof(material)-sizeof(MmsHeader))||
        !reference.getReferenceCapabilities||!reference.createReferenceSceneSource||!reference.readMaterialReferences||!reference.readToonTable)return false;
    state.api17=reference;
    Log("EVALUATION_NEGOTIATION_PASS"); return true;
}
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    if (!host || !info || !out) return MMS_E_ARGUMENT;
    state = {}; state.host = *host; *out = nullptr;
    const std::string id(info->pluginId.data, static_cast<size_t>(info->pluginId.bytes));
    state.holdOk = id.find("holdok") != std::string::npos;
    if (!Negotiate()) return MMS_E_FAILED;
    MmsEvaluationSourceQueryV1 query{H<MmsEvaluationSourceQueryV1>(), {{11, 22}, 1}, 0, 0, 0};
    const auto result = id.find("legacy") != std::string::npos
        ? state.api.createSource(state.api.context, &query, &state.source)
        : state.api12.createSourceChannels(state.api12.context, &query,
            id.find("v11-default") != std::string::npos ? 3u : 7u, &state.source);
    if (result) return result;
    Log("EVALUATION_SOURCE_CREATED");
    if (id.find("failnull") != std::string::npos) { Log("EVALUATION_FAIL_NULL"); return MMS_E_FAILED; }
    *out = &state;
    if (id.find("failinstance") != std::string::npos) { Log("EVALUATION_FAIL_INSTANCE"); return MMS_E_FAILED; }
    return MMS_OK;
}
MmsResult MMS_CALL Start(void*) { Log("EVALUATION_START"); return MMS_OK; }
void MMS_CALL Event(void*, const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void*) {
    ++state.stops;
    if (state.source && !state.holdOk) { Log("EVALUATION_STOP_BUSY"); return MMS_E_BUSY; }
    Log("EVALUATION_STOP_OK"); return MMS_OK;
}
void MMS_CALL Destroy(void*) { Log("EVALUATION_DESTROY"); }
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet(MmsEvaluationApiV1* api, MmsHandle* source) {
    if (!api || !source) return MMS_E_ARGUMENT;
    *api = state.api; *source = state.source; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestQuery(MmsEvaluationApiV1* out) {
    return state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 0, sizeof(*out), out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet11(MmsEvaluationApiV1_1* out) {
    if (!out) return MMS_E_ARGUMENT;
    *out = state.api11; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestQuery11(MmsEvaluationApiV1_1* out) {
    return state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 1, sizeof(*out), out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet12(MmsEvaluationApiV1_2* out) {
    if (!out) return MMS_E_ARGUMENT;
    *out = state.api12; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestQuery12(MmsEvaluationApiV1_2* out) {
    return state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 2, sizeof(*out), out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet13(MmsEvaluationApiV1_3* out) {
    if (!out) return MMS_E_ARGUMENT;
    *out = state.api13; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet14(MmsEvaluationApiV1_4* out) {
    if (!out) return MMS_E_ARGUMENT;
    *out = state.api14; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestSceneCreate(const MmsEvaluationSceneQueryV1* query, MmsHandle* out) {
    return state.api14.createSceneSource(state.api14.context, query, out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestSceneEvaluate(const MmsEvaluationFrameQueryV1* query, MmsHandle* out) {
    return state.api14.evaluateFrame(state.api14.context, query, out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet15(MmsEvaluationApiV1_5* out) {
    if(!out)return MMS_E_ARGUMENT;*out=state.api15;return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestMeshCreate(const MmsEvaluationSceneQueryV1* query,MmsHandle* out) {
    return state.api15.createMeshSceneSource(state.api15.context,query,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestMeshIndices(MmsHandle source,uint32_t* out) {
    return state.api15.readMeshIndices(state.api15.context,source,0,0,3,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestMeshVertices(MmsHandle frame,MmsEvaluationMeshVertexV1* out) {
    return state.api15.readMeshVertices(state.api15.context,frame,0,0,2,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet16(MmsEvaluationApiV1_6* out) {
    if(!out)return MMS_E_ARGUMENT;*out=state.api16;return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestMaterialCreate(const MmsEvaluationSceneQueryV1* query,MmsHandle* out) {
    return state.api16.createMaterialSceneSource(state.api16.context,query,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestMaterialRead(MmsHandle frame,MmsEvaluationMaterialV1* out) {
    return state.api16.readFrameMaterials(state.api16.context,frame,0,0,1,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestGet17(MmsEvaluationApiV1_7* out) {
    if(!out)return MMS_E_ARGUMENT;*out=state.api17;return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestReferenceCreate(const MmsEvaluationSceneQueryV1* query,MmsHandle* out) {
    return state.api17.createReferenceSceneSource(state.api17.context,query,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestReferenceRead(MmsHandle source,MmsEvaluationMaterialReferencesV1* out) {
    return state.api17.readMaterialReferences(state.api17.context,source,0,0,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestReferenceToon(MmsHandle source,MmsEvaluationToonTableV1* out) {
    return state.api17.readToonTable(state.api17.context,source,0,out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestQuery13(MmsEvaluationApiV1_3* out) {
    return state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 3, sizeof(*out), out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestParentState(uint32_t index, MmsTime time, MmsParentStateV1* out) {
    return state.api13.sampleParentState(state.api13.context, state.source, index, time, out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestParentLegacy(uint32_t index, MmsTime time, MmsParentSelectionV1* out) {
    return state.api12.sampleParent(state.api12.context, state.source, index, time, out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationTestRelease() {
    if (!state.source) return MMS_OK;
    const auto result = state.api.release(state.api.context, state.source);
    if (result == MMS_OK || result == MMS_E_NOT_FOUND) state.source = 0;
    return result;
}
extern "C" __declspec(dllexport) uint32_t MMS_CALL MmsEvaluationTestStopCount() { return state.stops; }
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor) return MMS_E_VERSION;
    *out = {H<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy}; return MMS_OK;
}
