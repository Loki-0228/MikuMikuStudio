#pragma once
// Private POD protocol for a real DLL. All exports are UI-thread entry points;
// worker probes start and join their own threads before returning.
#include <mms/property_refresh.h>
#include <mms/refresh_dependency.h>
#define SCOPED_PROPERTY_TARGETS 3u
#define SCOPED_PROPERTY_PROPERTIES 3u
#define SCOPED_PROPERTY_TRACE 256u
#pragma pack(push, 8)
typedef struct ScopedPropertyProbeEvent {
    uint32_t phase, target;
    int64_t value;
    uint64_t revision;
} ScopedPropertyProbeEvent;
typedef struct ScopedPropertyProbeState {
    uint32_t size, scope, events, overflow, edgeMask, workerWasWorker, explicitRequests;
    uint32_t reads[3], successes[3], releases[3], prepares[3], states[3], stages;
    uint32_t readDepth, maxReadDepth, identity, callbacksUi, readOnlyChecks, outputsPreserved;
    uint32_t oldSize, oldMinor, newSize, newMinor, negotiationPreserved, queryOutputPreserved;
    uint32_t stops, readsAtStop, stopOutputPreserved, failMask, legacySelf;
    int64_t values[3], lastRead[3], prepared[3], stateValues[3], display[3], sourceValue;
    uint64_t sourceRevision, preparedRevisions[3], stateRevisions[3], displayRevisions[3];
    MmsResult editQuery, shortQuery, query, oldQuery, workerQuery, workerCaps, workerRef, workerSubscribe, workerUnsubscribe;
    MmsResult readNotify, readWorkerNotify, readWorkerQuery, readSubscribe, readUnsubscribe, readGraph, readWorkerPublish;
    MmsResult stopQuery, stopNotify, stopUnsubscribe;
    MmsHandle properties[3], targets[3], seedHandle, command, edges[3], source, bindings[3], providers[2], subscription, heldSnapshot;
    MmsUiPropertyRefV1 refs[3];
    MmsUiTargetRefV1 seed, targetRefs[3];
    MmsUiPropertyRefreshCapabilitiesV1 oldCaps;
    MmsUiPropertyRefreshScopedCapabilitiesV1 capabilities;
    MmsUiPropertyRefreshScopedSubscriptionStateV1 subscriptionState;
    MmsUiPropertyRefreshSubscriptionStateV1 oldState;
    ScopedPropertyProbeEvent trace[SCOPED_PROPERTY_TRACE];
} ScopedPropertyProbeState;
#pragma pack(pop)
typedef MmsResult (MMS_CALL *ScopedPropertyBeginFn)(uint32_t scope, uint32_t edgeMask, uint32_t flags);
typedef MmsResult (MMS_CALL *ScopedPropertySetFn)(uint32_t property, int64_t value);
typedef MmsResult (MMS_CALL *ScopedPropertyNotifyFn)(uint32_t count, uint32_t worker);
typedef MmsResult (MMS_CALL *ScopedPropertyBurstFn)(int64_t first, uint32_t count);
typedef MmsResult (MMS_CALL *ScopedPropertyMaskFn)(uint32_t value);
typedef MmsResult (MMS_CALL *ScopedPropertyQueryFn)(ScopedPropertyProbeState*);
typedef MmsResult (MMS_CALL *ScopedPropertySimpleFn)(void);
