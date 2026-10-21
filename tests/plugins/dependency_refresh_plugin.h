#pragma once
// Private exports of the test DLL. Begin, SetState, RemoveEdge, Query and
// CheckEdges run on the fixture UI thread. Request and Release launch and join
// a real worker; the worker uses only published target identities and tickets.
#include <mms/refresh_dependency.h>

#define DEPENDENCY_PROBE_BUTTONS 4u
#define DEPENDENCY_PROBE_EDGES 3u
#define DEPENDENCY_PROBE_TICKETS 2u

typedef struct DependencyRefreshProbeState {
    uint32_t size, requestCount, sequence, stopCount;
    uint32_t calls[DEPENDENCY_PROBE_BUTTONS];
    uint32_t callSequence[DEPENDENCY_PROBE_BUTTONS];
    uint32_t stopHeldTickets, requestWasWorker, releaseWasWorker, reserved;
    MmsResult workerConnectResult, workerRegisterResult;
    MmsResult edgeCleanup[DEPENDENCY_PROBE_EDGES];
    MmsResult ticketQuery[DEPENDENCY_PROBE_TICKETS];
    MmsHandle buttons[DEPENDENCY_PROBE_BUTTONS];
    MmsHandle edges[DEPENDENCY_PROBE_EDGES];
    MmsHandle tickets[DEPENDENCY_PROBE_TICKETS];
    MmsUiTargetRefV1 targets[DEPENDENCY_PROBE_BUTTONS];
    MmsUiRefreshStateV1 ticketStates[DEPENDENCY_PROBE_TICKETS];
    MmsUiRefreshDependencyCapabilitiesV1 capabilities;
} DependencyRefreshProbeState;

typedef MmsResult (MMS_CALL *DependencyRefreshProbeSimpleFn)(void);
typedef MmsResult (MMS_CALL *DependencyRefreshProbeRequestFn)(uint32_t count);
typedef MmsResult (MMS_CALL *DependencyRefreshProbeIndexFn)(uint32_t index);
typedef MmsResult (MMS_CALL *DependencyRefreshProbeSetStateFn)(uint32_t index, uint32_t enabled, uint32_t checked);
typedef MmsResult (MMS_CALL *DependencyRefreshProbeQueryFn)(DependencyRefreshProbeState*);

#ifdef __cplusplus
extern "C" {
#endif
MmsResult MMS_CALL DependencyProbeBegin(void);
MmsResult MMS_CALL DependencyProbeSetState(uint32_t index, uint32_t enabled, uint32_t checked);
MmsResult MMS_CALL DependencyProbeRequest(uint32_t count);
MmsResult MMS_CALL DependencyProbeQuery(DependencyRefreshProbeState*);
MmsResult MMS_CALL DependencyProbeRelease(uint32_t index);
MmsResult MMS_CALL DependencyProbeRemoveEdge(uint32_t index);
MmsResult MMS_CALL DependencyProbeCheckEdges(void);
#ifdef __cplusplus
}
#endif
