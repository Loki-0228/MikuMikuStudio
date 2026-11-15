#include "settings_fixture_plugin.hpp"

namespace {
template<class T> MmsHeader Header() { return {sizeof(T), 1, 0}; }
template<size_t N> MmsString Text(const char (&s)[N]) { return {s, N - 1}; }

struct State {
    const MmsHostV1* host = nullptr;
    MmsSettingsApiV1 settings{};
    SettingsFixtureObservations observed{};
} state;

MmsResult Probe() {
    uint64_t bytes = 0;
    return state.settings.read(state.settings.context, nullptr, 0, &bytes);
}
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1*, void** out) {
    if (!host || !out) return MMS_E_ARGUMENT;
    *out = nullptr;
    state = {};
    state.host = host;
    state.observed.query = host->queryService(
        host->context, Text("mms.settings"), 1, 0, sizeof(state.settings), &state.settings);
    if (state.observed.query == MMS_OK) {
        if (state.settings.h.size != sizeof(state.settings) || state.settings.h.major != 1 ||
            state.settings.h.minor != 0 || !state.settings.context ||
            !state.settings.read || !state.settings.compareAndMerge) return MMS_E_VERSION;
        MmsSettingsApiV1 sentinel{};
        sentinel.h.size = 91;
        if (host->queryService(host->context, Text("mms.settings"), 1, 1, sizeof(sentinel), &sentinel) != MMS_E_VERSION ||
            host->queryService(host->context, Text("mms.settings"), 1, 0, sizeof(sentinel) - 1, &sentinel) != MMS_E_ARGUMENT ||
            sentinel.h.size != 91 || sentinel.context || sentinel.read || sentinel.compareAndMerge) return MMS_E_FAILED;
        state.observed.createRead = Probe();
    }
    *out = &state;
    return MMS_OK;
}
MmsResult MMS_CALL Start(void*) {
    if (state.observed.query == MMS_OK) {
        // A valid patch from Start must be rejected, not silently merged.
        state.observed.startMerge = state.settings.compareAndMerge(
            state.settings.context, {}, Text(R"({"outsideTask":true})"));
    }
    return MMS_OK;
}
void MMS_CALL Event(void*, const MmsEventV1* event) {
    switch (event->kind) {
    case MMS_EVENT_UI_READY:
        if (state.observed.query == MMS_OK) state.observed.eventRead = Probe();
        break;
    case MMS_EVENT_UI_CLOSING:
        if (state.observed.query == MMS_OK) state.observed.closingRead = Probe();
        break;
    case MMS_EVENT_PROJECT_OPENED:
    case MMS_EVENT_PROJECT_CLOSED:
    case MMS_EVENT_PROJECT_SAVED:
    case MMS_EVENT_SCENE_RESET:
    case MMS_EVENT_LAYOUT_CHANGED:
    case MMS_EVENT_UI_REGISTRY_CHANGED:
        ++state.observed.unexpectedEvents;
        break;
    default:
        break;
    }
}
void MMS_CALL Run(void* context) {
    auto& request = *static_cast<SettingsFixtureRequest*>(context);
    request.result = MmsSettingsFixtureDirect(&request);
    request.completed = true;
}
void MMS_CALL Cancel(void* context) { Run(context); } // cached table must deny a stop-time cancellation
MmsResult MMS_CALL Stop(void*) {
    if (state.observed.query == MMS_OK && (state.observed.closingRead != MMS_E_NOT_READY ||
        state.settings.compareAndMerge(state.settings.context, {}, Text("{}")) != MMS_E_NOT_READY))
        return MMS_E_FAILED;
    return MMS_OK;
}
void MMS_CALL Destroy(void*) {}
}

extern "C" const SettingsFixtureObservations* MMS_CALL MmsSettingsFixtureObserve() {
    return &state.observed;
}
extern "C" MmsResult MMS_CALL MmsSettingsFixtureDirect(SettingsFixtureRequest* request) {
    if (!request) return MMS_E_ARGUMENT;
    if (state.observed.query) return state.observed.query;
    return request->op == SettingsFixtureRequest::Op::Read
        ? state.settings.read(state.settings.context, request->buffer, request->capacity, &request->bytes)
        : state.settings.compareAndMerge(state.settings.context, request->expected, request->patch);
}
extern "C" MmsResult MMS_CALL MmsSettingsFixtureQueue(SettingsFixtureRequest* request) {
    if (!request) return MMS_E_ARGUMENT;
    if (state.observed.query) return state.observed.query;
    MmsHandle handle = 0;
    return state.host->enqueueUi(state.host->context, Run, Cancel, request, &handle);
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor != 0) return MMS_E_VERSION;
    *out = {Header<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy};
    return MMS_OK;
}
