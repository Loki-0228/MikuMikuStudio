#pragma once
// Private POD protocol for the actual property-refresh DLL fixture. Exports
// are called on its creating UI thread; worker probes launch and join locally.
#include <mms/property_refresh.h>
#define PROPERTY_REFRESH_PROBE_TRACE 32u
#pragma pack(push,8)
typedef struct PropertyRefreshProbeState {
    uint32_t size,reads,releases,readDepth,maxReadDepth,readWasUi,readIdentity;
    uint32_t readOnlyChecks,readOnlyDenied,readOnlyOutputPreserved,commands,notifies,workerWasWorker;
    uint32_t workerOutputPreserved,badOutputPreserved,stops,readsAtStop,stopOutputPreserved,traceOverflow;
    int64_t value,lastRead,traceValues[PROPERTY_REFRESH_PROBE_TRACE];
    MmsResult editQuery,commandNotify,readNotify,readWorkerNotify,readWorkerQuery,readSubscribe,readUnsubscribe,readMutation;
    MmsResult workerQuery,workerCaps,workerRef,workerSubscribe,workerSdkSubscribe,workerUnsubscribe;
    MmsResult badRef,badSubscribe,badQuery,subscriptionQuery,stopQuery,stopNotify,stopUnsubscribe,stopRelease;
    MmsHandle property,input,command,button,subscription,originalSubscription,heldSnapshot;
    MmsUiTargetRefV1 target;
    MmsUiPropertyRefV1 propertyRef;
    MmsUiPropertyRefreshCapabilitiesV1 capabilities;
    MmsUiPropertyRefreshSubscriptionStateV1 subscriptionState,workerState;
} PropertyRefreshProbeState;
#pragma pack(pop)
typedef MmsResult (MMS_CALL *PropertyRefreshProbeBeginFn)(uint32_t flags);
typedef MmsResult (MMS_CALL *PropertyRefreshProbeBurstFn)(int64_t first,uint32_t count);
typedef MmsResult (MMS_CALL *PropertyRefreshProbeValueFn)(int64_t value);
typedef MmsResult (MMS_CALL *PropertyRefreshProbeGetValueFn)(int64_t* out);
typedef MmsResult (MMS_CALL *PropertyRefreshProbeQueryFn)(PropertyRefreshProbeState* out);
typedef MmsResult (MMS_CALL *PropertyRefreshProbeSimpleFn)(void);
