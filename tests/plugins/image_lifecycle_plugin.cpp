// Deliberately retain CPU image mappings across stop to exercise host lifetime.
#include <mms/render_image.h>
#include <cstring>
#include <string>

namespace {
template<class T> MmsHeader H() { return {sizeof(T), 1, 0}; }
MmsString Text(const char* s) { return {s, std::strlen(s)}; }
struct State {
    MmsHostV1 host{};
    MmsRenderImageApiV1 images{};
    MmsHandle image = 0;
    MmsRenderImageMapV1 map{};
    bool noMap = false;
    uint32_t stops = 0;
} state;
void Log(const char* value) { state.host.log(state.host.context, 1, Text(value)); }
bool Negotiation() {
    MmsRenderImageApiV1 output;
    std::memset(&output, 0x5a, sizeof(output));
    const auto before = output;
    const auto query = [&](uint32_t major, uint32_t minor, uint32_t bytes) {
        return state.host.queryService(state.host.context, Text("mms.render.image"), major, minor, bytes, &output);
    };
    if (query(2, 0, sizeof(output)) != MMS_E_VERSION || std::memcmp(&output, &before, sizeof(output))) return false;
    if (query(1, 1, sizeof(output)) != MMS_E_VERSION || std::memcmp(&output, &before, sizeof(output))) return false;
    if (query(1, 0, sizeof(output) - 1) != MMS_E_ARGUMENT || std::memcmp(&output, &before, sizeof(output))) return false;
    if (state.host.queryService(state.host.context, Text("mms.render.image"), 1, 0, sizeof(output), nullptr) != MMS_E_ARGUMENT) return false;
    if (query(1, 0, sizeof(output)) != MMS_OK || output.h.size != sizeof(output) || output.h.major != 1 || output.h.minor != 0) return false;
    state.images = output;
    if (!output.context || !output.getCapabilities || !output.createCpu || !output.describe || !output.retain || !output.mapRead || !output.unmap || !output.release) return false;
    Log("IMAGE_NEGOTIATION_PASS");
    return true;
}
MmsResult Prepare() {
    const unsigned char pixels[8] = {17, 34, 51, 255, 68, 85, 102, 127};
    const MmsRenderImageDescV1 desc{H<MmsRenderImageDescV1>(), 2, 1,
        MMS_IMAGE_RGBA8, MMS_IMAGE_COLOR_SRGB, MMS_IMAGE_ALPHA_STRAIGHT, 0, 8};
    const auto result = state.images.createCpu(state.images.context, &desc, {pixels, sizeof(pixels)}, &state.image);
    if (result != MMS_OK) return result;
    if (!state.noMap) {
        const auto mapped = state.images.mapRead(state.images.context, state.image, &state.map);
        if (mapped != MMS_OK) return mapped;
    }
    Log("IMAGE_PREPARED");
    return MMS_OK;
}
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    if (!host || !info || !out) return MMS_E_ARGUMENT;
    state = {};
    state.host = *host;
    *out = nullptr;
    const std::string id(info->pluginId.data, static_cast<size_t>(info->pluginId.bytes));
    state.noMap = id.find("nomap") != std::string::npos;
    if (!Negotiation()) return MMS_E_FAILED;
    if (id.find("failcreate") != std::string::npos) {
        const auto prepared = Prepare();
        if (prepared != MMS_OK) return prepared;
        Log("IMAGE_CREATE_FAIL_NULL");
        return MMS_E_FAILED;
    }
    *out = &state;
    return MMS_OK;
}
MmsResult MMS_CALL Start(void*) { return Prepare(); }
void MMS_CALL Event(void*, const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void*) { ++state.stops; Log("IMAGE_STOP_OK"); return MMS_OK; }
void MMS_CALL Destroy(void*) { Log("IMAGE_DESTROY"); }
}

extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsImageTestGet(
    MmsRenderImageApiV1* api, MmsHandle* image, MmsRenderImageMapV1* map) {
    if (!api || !image || !map) return MMS_E_ARGUMENT;
    *api = state.images;
    *image = state.image;
    *map = state.map;
    return MMS_OK;
}
extern "C" __declspec(dllexport) uint32_t MMS_CALL MmsImageTestStopCount() {
    return state.stops;
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(
    uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor) return MMS_E_VERSION;
    *out = {H<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy};
    return MMS_OK;
}
