#pragma once
// Private test-only exports of data_refresh_plugin.dll. Publish/ReleaseHold
// launch and join an actual std::thread. EnqueueNormal permits worker callers;
// remaining operations perform their host UI work on the calling UI thread.
#include <mms/refresh.h>
#define DATA_PROBE_PANEL 1u
#define DATA_PROBE_BUTTON 2u
#define DATA_PROBE_FAIL_PREPARE 1u
#define DATA_PROBE_NO_WRITE 2u
#define DATA_PROBE_DOUBLE_WRITE 4u
#define DATA_PROBE_THROW_MEMORY 8u
#define DATA_PROBE_ENQUEUE 16u
typedef struct DataRefreshProbeEditState {
    uint32_t calls;
    MmsResult begin, undo, redo;
    MmsHandle originalOutput, beginOutput;
} DataRefreshProbeEditState;
typedef struct DataRefreshProbeTaskState {
    uint32_t calls;
    MmsResult mutate, dataRequest, stateRequest, enqueueChild;
    DataRefreshProbeEditState edit;
} DataRefreshProbeTaskState;
typedef struct DataRefreshProbeState {
    uint32_t size, targetKind, flags, prepareCount, stateCount, paintCount, stopCount;
    uint32_t stopHeldTicket, stopHeldDisplay, panelCreated, panelDestroyed;
    int32_t sourceValue, preparedValue, paintValue, stateValue, previousDisplayValue, heldValue;
    MmsResult sourceRead, oldDisplayRead, directMutation, recursiveData, recursiveState;
    MmsResult prepareWrite, secondWrite, publishResult, heldReleaseResult, ticketQueryResult;
    MmsResult enqueueRun, enqueueCancel, selfCancel;
    MmsResult editQuery;
    uint32_t editAvailable;
    uint64_t editExpectedRevision;
    DataRefreshProbeEditState edit;
    uint64_t sourceRevision, preparedRevision, sourceReadRevision, displayRevision;
    uint64_t sequence, prepareSequence, stateSequence, paintSequence;
    MmsHandle menu, button, panel, source, binding, ticket, heldLease;
    uintptr_t panelWindow;
    MmsUiTargetRefV1 target;
    MmsUiRefreshStateV1 ticketState;
    // 0 normal task; 1 self-cancel callback; 2 normal child; 3 cancel child.
    DataRefreshProbeTaskState tasks[4];
} DataRefreshProbeState;
typedef MmsResult (MMS_CALL *DataRefreshProbeBeginFn)(uint32_t targetKind);
typedef MmsResult (MMS_CALL *DataRefreshProbePublishFn)(int32_t);
typedef MmsResult (MMS_CALL *DataRefreshProbeRequestFn)(uint32_t kinds,uint32_t flags);
typedef MmsResult (MMS_CALL *DataRefreshProbeQueryFn)(DataRefreshProbeState*);
typedef MmsResult (MMS_CALL *DataRefreshProbeSimpleFn)(void);
typedef void (MMS_CALL *DataRefreshProbePrepareHookFn)(void*);
typedef MmsResult (MMS_CALL *DataRefreshProbeSetPrepareHookFn)(DataRefreshProbePrepareHookFn,void*);
#ifdef __cplusplus
extern "C" {
#endif
// Start registers both real targets. Begin binds the selected existing target
// when the native adapter has mounted it. First Publish is explicitly required.
MmsResult MMS_CALL ProbeBegin(uint32_t targetKind);
MmsResult MMS_CALL ProbePublish(int32_t);
MmsResult MMS_CALL ProbeRequest(uint32_t kinds,uint32_t flags);
MmsResult MMS_CALL ProbeQuery(DataRefreshProbeState*);
MmsResult MMS_CALL ProbeReleaseTicket(void);
MmsResult MMS_CALL ProbeAcquireHold(void);
MmsResult MMS_CALL ProbeReleaseHold(void);
MmsResult MMS_CALL ProbeRemoveTarget(void);
MmsResult MMS_CALL ProbeSetPrepareHook(DataRefreshProbePrepareHookFn,void*);
MmsResult MMS_CALL ProbeEnqueueNormal(void);
#ifdef __cplusplus
}
#endif
