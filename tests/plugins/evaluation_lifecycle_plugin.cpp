#include <mms/evaluation.h>
#include <cstring>
#include <string>

namespace {
template<class T> MmsHeader H() { return {sizeof(T), 1, 0}; }
MmsString Text(const char* text) { return {text, std::strlen(text)}; }
struct State {
    MmsHostV1 host{};
    MmsEvaluationApiV1 api{};
    MmsHandle source = 0;
    uint32_t stops = 0;
    bool holdOk = false;
} state;
void Log(const char* text) { state.host.log(state.host.context, 1, Text(text)); }
bool Negotiate() {
    MmsEvaluationApiV1 out;
    std::memset(&out, 0x5a, sizeof(out)); const auto before = out;
    const auto query = [&](uint32_t major, uint32_t minor, uint32_t bytes) {
        return state.host.queryService(state.host.context, Text("mms.scene.evaluate"), major, minor, bytes, &out);
    };
    if (query(2, 0, sizeof(out)) != MMS_E_VERSION || std::memcmp(&out, &before, sizeof(out))) return false;
    if (query(1, 1, sizeof(out)) != MMS_E_VERSION || std::memcmp(&out, &before, sizeof(out))) return false;
    if (query(1, 0, sizeof(out) - 1) != MMS_E_ARGUMENT || std::memcmp(&out, &before, sizeof(out))) return false;
    if (state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 0, sizeof(out), nullptr) != MMS_E_ARGUMENT) return false;
    if (query(1, 0, sizeof(out)) != MMS_OK || out.h.size != sizeof(out) || out.h.major != 1 || out.h.minor) return false;
    if (!out.context || !out.getCapabilities || !out.createSource || !out.describeSource || !out.boneAt || !out.sampleBone || !out.retain || !out.release) return false;
    state.api = out;
    Log("EVALUATION_NEGOTIATION_PASS"); return true;
}
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    if (!host || !info || !out) return MMS_E_ARGUMENT;
    state = {}; state.host = *host; *out = nullptr;
    const std::string id(info->pluginId.data, static_cast<size_t>(info->pluginId.bytes));
    state.holdOk = id.find("holdok") != std::string::npos;
    if (!Negotiate()) return MMS_E_FAILED;
    MmsEvaluationSourceQueryV1 query{H<MmsEvaluationSourceQueryV1>(), {{11, 22}, 1}, 0, 0, 0};
    const auto result = state.api.createSource(state.api.context, &query, &state.source);
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
