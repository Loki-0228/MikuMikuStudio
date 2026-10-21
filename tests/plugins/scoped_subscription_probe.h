#pragma once
#include <mms/refresh_subscription.h>
#include <mms/refresh_dependency.h>
#define SCOPED_PROBE_BUTTONS 4u
#define SCOPED_PROBE_TRACE 128u
#pragma pack(push,8)
typedef struct ScopedSubscriptionProbeEvent {
    uint32_t phase,target;
    int32_t value;
    uint32_t reserved;
    uint64_t revision;
} ScopedSubscriptionProbeEvent;
typedef struct ScopedSubscriptionProbeState {
    uint32_t size,scope,edgeMask,events,overflow,prepared[SCOPED_PROBE_BUTTONS],states[SCOPED_PROBE_BUTTONS];
    uint32_t identity,uiCallbacks,readOnlyChecks,readOnlyDenied,outputsPreserved,workerWasWorker,explicitRequests,stops,preparesAtStop;
    uint32_t negotiationPreserved,legacySize,legacyMinor,newSize,newMinor;
    int32_t sourceValues[2],preparedValues[SCOPED_PROBE_BUTTONS],stateValues[SCOPED_PROBE_BUTTONS],displayValues[SCOPED_PROBE_BUTTONS];
    uint64_t sourceRevisions[2],preparedRevisions[SCOPED_PROBE_BUTTONS],stateRevisions[SCOPED_PROBE_BUTTONS],displayRevisions[SCOPED_PROBE_BUTTONS];
    MmsResult editQuery,truncatedQuery,query,legacyQuery,readonlyEdge,readonlyUnedge,readonlySubscribe,readonlyPublish,readonlyWorkerPublish,readonlyWorkerQuery;
    MmsResult stopQuery,stopUnsubscribe,stopOutputPreserved;
    MmsHandle seedHandle,buttons[SCOPED_PROBE_BUTTONS],sources[2],bindings[SCOPED_PROBE_BUTTONS],edges[3],subscription,heldSnapshot;
    MmsUiTargetRefV1 seed,targets[SCOPED_PROBE_BUTTONS];
    MmsUiRefreshSubscriptionCapabilitiesV1 legacyCaps,capabilities;
    MmsUiRefreshScopedSubscriptionStateV1 subscriptionState;
    MmsUiRefreshSubscriptionStateV1 legacyState;
    ScopedSubscriptionProbeEvent trace[SCOPED_PROBE_TRACE];
} ScopedSubscriptionProbeState;
#pragma pack(pop)
typedef MmsResult (MMS_CALL *ScopedProbeBeginFn)(uint32_t scope,uint32_t edgeMask);
typedef MmsResult (MMS_CALL *ScopedProbePublishFn)(uint32_t source,int32_t first,uint32_t count);
typedef MmsResult (MMS_CALL *ScopedProbeGraphFn)(uint32_t mask);
typedef MmsResult (MMS_CALL *ScopedProbeQueryFn)(ScopedSubscriptionProbeState*);
typedef MmsResult (MMS_CALL *ScopedProbeSimpleFn)(void);
