#ifndef MMS_REFRESH_BINDING_H
#define MMS_REFRESH_BINDING_H
#include "refresh.h"
#pragma pack(push, 8)
typedef struct MmsUiDataCapabilitiesV1 {
    MmsHeader h;
    uint32_t maxSourcesPerPlugin, maxBindingsPerPlugin, maxDisplayLeasesPerPlugin, maxBuildersPerPlugin;
    uint64_t maxSourceBytes, maxDisplayBytes, maxRetainedBytesPerPlugin;
    uint32_t maxContributionsPerPlugin, reserved;
} MmsUiDataCapabilitiesV1;
typedef struct MmsUiDataSourceDescV1 {
    MmsHeader h;
    MmsString id;
    uint64_t maxBytes;
    uint32_t flags, reserved;
} MmsUiDataSourceDescV1;
typedef struct MmsUiDataSourcePublishV1 {
    MmsHeader h;
    MmsHandle source;
    uint64_t expectedRevision;
    MmsBytes bytes;
    uint32_t flags, reserved;
} MmsUiDataSourcePublishV1;
typedef struct MmsUiDataSourceStateV1 {
    MmsHeader h;
    MmsHandle source;
    uint64_t revision, projectEpoch, byteCount;
} MmsUiDataSourceStateV1;
typedef struct MmsUiDataPrepareV1 {
    MmsHeader h;
    MmsUiTargetRefV1 target;
    MmsHandle source;
    uint64_t sourceRevision;
    MmsBytes sourceBytes;
    MmsHandle builder;
    uint64_t previousRevision;
} MmsUiDataPrepareV1;
typedef MmsResult (MMS_CALL *MmsUiDataPrepareCallbackV1)(void*, const MmsUiDataPrepareV1*);
typedef struct MmsUiDataBindingDescV1 {
    MmsHeader h;
    MmsUiTargetRefV1 target;
    MmsHandle source;
    void* context;
    MmsUiDataPrepareCallbackV1 prepare;
    uint64_t maxDisplayBytes;
    uint32_t flags, reserved;
} MmsUiDataBindingDescV1;
typedef struct MmsUiDataDisplayV1 {
    MmsHeader h;
    MmsHandle lease, binding, source;
    uint64_t sourceRevision, displayRevision, projectEpoch;
    MmsBytes bytes;
} MmsUiDataDisplayV1;
/* queryService("mms.ui.refresh.bind", 1, 0, ...).
   Sources contain immutable HOST COPIES. publishSource/getSource and display
   acquire/release allow workers; other registrations are UI-thread only.
   expectedRevision is exact, including zero for the first publication.
   DATA uses the existing refresh.request/query/cancel/release ticket service.
   Capture accepts the exact current source revision; accepted contributions
   retain that snapshot across later publications. Older uncaptured revisions
   return STALE. Empty bytes are a valid model.
   prepare runs read-only on the UI thread. It calls writePrepared once using
   the short-lived builder, then returns. Null prepare copies source bytes.
   The host publishes by swapping its immutable model only after success and
   revalidation. Painting reads acquireDisplay; no business callback runs there.
   Stopping forbids acquisition/publication, but existing leases must be released. */
typedef struct MmsUiRefreshBindingApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiDataCapabilitiesV1*);
    MmsResult (MMS_CALL *registerSource)(void*, const MmsUiDataSourceDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *publishSource)(void*, const MmsUiDataSourcePublishV1*, MmsUiDataSourceStateV1*);
    MmsResult (MMS_CALL *getSource)(void*, MmsHandle, MmsUiDataSourceStateV1*);
    MmsResult (MMS_CALL *unregisterSource)(void*, MmsHandle);
    MmsResult (MMS_CALL *bind)(void*, const MmsUiDataBindingDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *unbind)(void*, MmsHandle);
    MmsResult (MMS_CALL *writePrepared)(void*, MmsHandle, MmsBytes);
    MmsResult (MMS_CALL *acquireDisplay)(void*, MmsHandle, MmsUiDataDisplayV1*);
    MmsResult (MMS_CALL *releaseDisplay)(void*, MmsHandle);
} MmsUiRefreshBindingApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiDataCapabilitiesV1) == 56, "MMS DATA capabilities");
static_assert(sizeof(MmsUiDataSourceDescV1) == 40, "MMS DATA source descriptor");
static_assert(sizeof(MmsUiDataSourcePublishV1) == 48, "MMS DATA source publication");
static_assert(sizeof(MmsUiDataSourceStateV1) == 40, "MMS DATA source state");
static_assert(sizeof(MmsUiDataPrepareV1) == 88, "MMS DATA prepare input");
static_assert(sizeof(MmsUiDataBindingDescV1) == 80, "MMS DATA binding descriptor");
static_assert(sizeof(MmsUiDataDisplayV1) == 72, "MMS DATA display lease");
static_assert(sizeof(MmsUiRefreshBindingApiV1) == 96, "MMS DATA binding API");
#endif
#endif
