#include <mms/evaluation.h>
#include <cstring>
#pragma comment(linker, "/EXPORT:MMSPluginGetApi")
namespace {
MmsEvaluationApiV1 api{};
MmsEvaluationApiV1_1 api11{};
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1*, void** out) {
    if (!host || !out) return MMS_E_ARGUMENT;
    constexpr char service[] = "mms.scene.evaluate";
    const auto status = host->queryService(host->context, {service, sizeof(service)-1}, 1, 0, sizeof(api), &api);
    if (status != MMS_OK) return status;
    const auto extended = host->queryService(host->context, {service, sizeof(service)-1}, 1, 1, sizeof(api11), &api11);
    if (extended == MMS_OK) *out = &api;
    return extended;
}
MmsResult MMS_CALL Start(void*) { return MMS_OK; }
void MMS_CALL Event(void*, const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void*) { return MMS_OK; }
void MMS_CALL Destroy(void*) { api = {}; api11 = {}; }
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi(MmsEvaluationApiV1* out) {
    if (!out || !api.context) return MMS_E_NOT_READY;
    *out = api; return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsEvaluationHostGetApi11(MmsEvaluationApiV1_1* out) {
    if (!out || !api11.context) return MMS_E_NOT_READY;
    *out = api11; return MMS_OK;
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor,
    uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor) return MMS_E_VERSION;
    *out = {{sizeof(*out),1,0},Create,Start,Event,Stop,Destroy}; return MMS_OK;
}
