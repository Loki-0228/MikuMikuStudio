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
    MmsHandle source = 0;
    uint32_t stops = 0;
    bool holdOk = false;
} state;
void Log(const char* text) { state.host.log(state.host.context, 1, Text(text)); }
bool Negotiate() {
    static_assert(sizeof(MmsEvaluationApiV1) == 72 && sizeof(MmsEvaluationApiV1_1) == 104);
    struct Buffer { MmsEvaluationApiV1_1 table; uint64_t guard; } buffer;
    std::memset(&buffer, 0x5a, sizeof(buffer)); const auto before = buffer;
    const auto query = [&](uint32_t major, uint32_t minor, uint32_t bytes) {
        return state.host.queryService(state.host.context, Text("mms.scene.evaluate"), major, minor, bytes, &buffer.table);
    };
    if (query(2, 0, 104) != MMS_E_VERSION || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 2, 104) != MMS_E_VERSION || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 1, 72) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 1, 103) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (query(1, 0, 71) != MMS_E_ARGUMENT || std::memcmp(&buffer, &before, sizeof(buffer))) return false;
    if (state.host.queryService(state.host.context, Text("mms.scene.evaluate"), 1, 0, 72, nullptr) != MMS_E_ARGUMENT) return false;
    if (query(1, 0, 72) != MMS_OK) return false;
    if (std::memcmp(reinterpret_cast<const char*>(&buffer) + 72, reinterpret_cast<const char*>(&before) + 72, sizeof(buffer) - 72)) return false;
    MmsEvaluationApiV1 out{}; std::memcpy(&out, &buffer.table, sizeof(out));
    if (out.h.size != 72 || out.h.major != 1 || out.h.minor) return false;
    if (!out.context || !out.getCapabilities || !out.createSource || !out.describeSource || !out.boneAt || !out.sampleBone || !out.retain || !out.release) return false;
    state.api = out;
    buffer = before;
    if (query(1, 1, 104) != MMS_OK || buffer.guard != before.guard) return false;
    const auto& extended = buffer.table;
    if (extended.h.size != 104 || extended.h.major != 1 || extended.h.minor != 1 ||
        extended.context != out.context || extended.getCapabilities != out.getCapabilities ||
        extended.createSource != out.createSource || extended.describeSource != out.describeSource ||
        extended.boneAt != out.boneAt || extended.sampleBone != out.sampleBone ||
        extended.retain != out.retain || extended.release != out.release ||
        !extended.createSourceChannels || !extended.describeSourceChannels || !extended.morphAt || !extended.sampleMorph) return false;
    state.api11 = extended;
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
        : state.api11.createSourceChannels(state.api11.context, &query,
            MMS_EVALUATION_CAP_BONE_CHANNELS | MMS_EVALUATION_CAP_MORPH_WEIGHTS, &state.source);
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
