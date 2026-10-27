#include <mms/evaluation.h>
#include <cstring>
#pragma comment(linker, "/EXPORT:MMSPluginGetApi")
namespace {
MmsEvaluationApiV1 api{};
MmsEvaluationApiV1_1 api11{};
MmsEvaluationApiV1_2 api12{};
MmsEvaluationApiV1_3 api13{};
MmsEvaluationApiV1_4 api14{};
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
    if (scene == MMS_OK) *out = &api;
    return scene;
}
MmsResult MMS_CALL Start(void*) { return MMS_OK; }
void MMS_CALL Event(void*, const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void*) { return MMS_OK; }
void MMS_CALL Destroy(void*) { api = {}; api11 = {}; api12 = {}; api13 = {}; api14 = {}; }
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
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor,
    uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor) return MMS_E_VERSION;
    *out = {{sizeof(*out),1,0},Create,Start,Event,Stop,Destroy}; return MMS_OK;
}
