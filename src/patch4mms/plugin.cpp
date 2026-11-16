#include <mms/plugin.h>
#include <mms/mme_on12.h>
#include <cstdio>
#include <cstring>
#include <new>
namespace { struct Instance { MmsHostV1 host; }; }
#pragma comment(linker, "/EXPORT:MMSPatch4MMSAutoStartV1")
extern "C" uint32_t MMS_CALL MMSPatch4MMSAutoStartV1() { return 1; }
#pragma comment(linker, "/EXPORT:MMSPluginGetApi")
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor,
    uint32_t bytes, MmsPluginApiV1* api) {
    if (major != 1 || minor != 0) return MMS_E_VERSION;
    if (!api || bytes < sizeof(*api)) return MMS_E_ARGUMENT;
    *api = {}; api->h = {sizeof(*api), 1, 0};
    api->create = [](const MmsHostV1* host, const MmsCreateInfoV1*, void** out) -> MmsResult {
        if (!host || !out) return MMS_E_ARGUMENT;
        *out = new(std::nothrow) Instance{*host}; return *out ? MMS_OK : MMS_E_MEMORY;
    };
    api->start = [](void* context) -> MmsResult {
        auto& host = static_cast<Instance*>(context)->host;
        constexpr char id[] = "mms.render.mme.on12";
        MmsMmeOn12ApiV1 api{};
        auto result = host.queryService(host.context, {id, sizeof(id)-1}, 1, 0, sizeof(api), &api);
        if (result != MMS_OK) return result;
        MmsMmeOn12StatusV1 status{{sizeof(status), 1, 0}};
        result = api.query(api.context, &status); if (result != MMS_OK) return result;
        char text[256]{};
        std::snprintf(text, sizeof(text), "Patch4MMS requested=%u actual=%u reason=%u enabled=%u mme=%u bridge=%u",
            status.requestedBackend, status.actualBackend, status.reason, status.startupRequestEnabled, status.mmeLoaded, status.bridgeSupported);
        host.log(host.context, 1, {text, std::strlen(text)}); return MMS_OK;
    };
    api->onEvent = [](void*, const MmsEventV1*) {};
    api->stop = [](void*) -> MmsResult { return MMS_OK; };
    api->destroy = [](void* context) { delete static_cast<Instance*>(context); };
    return MMS_OK;
}
