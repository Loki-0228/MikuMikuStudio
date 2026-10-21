#pragma once
// Private ABI of the real subscription test DLL. Exports run on the fixture's
// UI thread; Publish and CheckThreads launch and join their own worker.
#include <mms/refresh_subscription.h>
#define SUBSCRIPTION_PROBE_TRACE 32u
typedef struct SubscriptionRefreshProbeState {
    uint32_t size, prepareCount, stateCount, stopCount;
    uint32_t sequence, prepareSequence, stateSequence, prepareWasUi, prepareIdentity;
    uint32_t publishWasWorker, publishCount, explicitRequests, traceOverflow;
    uint32_t editAvailable, editCalls, editOutputPreserved, readOnlyOutputPreserved, everyReadOnlyDenied;
    uint32_t workerOutputPreserved, stopOutputPreserved, stopHeldDisplay, prepareAtStop;
    int32_t sourceValue, preparedValue, stateValue, displayValue, heldValue, previousValue;
    MmsResult editQuery, editBegin, editUndo, editRedo, prepareWrite, directMutation;
    MmsResult readOnlySubscribe, readOnlyUnsubscribe, readOnlyPublish;
    MmsResult publishResult, workerQuery, workerSubscribe, workerSdkSubscribe, workerUnsubscribe, workerCaps;
    MmsResult subscriptionQuery, displayQuery, ticketQuery, stopQuery, stopUnsubscribe, stopRelease, heldRelease;
    MmsHandle button, source, binding, subscription, originalSubscription, ticket, heldLease;
    uint64_t sourceRevision, preparedRevision, displaySourceRevision, displayRevision;
    uint64_t previousSourceRevision, heldSourceRevision, editExpectedRevision;
    int32_t traceValues[SUBSCRIPTION_PROBE_TRACE];
    uint64_t traceRevisions[SUBSCRIPTION_PROBE_TRACE];
    MmsUiTargetRefV1 target;
    MmsUiRefreshSubscriptionStateV1 subscriptionState, workerState;
    MmsUiRefreshStateV1 ticketState;
    MmsUiRefreshSubscriptionCapabilitiesV1 capabilities;
} SubscriptionRefreshProbeState;
typedef MmsResult (MMS_CALL *SubscriptionProbeBeginFn)(uint64_t historyRevision, uint32_t subscribeNow);
typedef MmsResult (MMS_CALL *SubscriptionProbePublishFn)(int32_t first, uint32_t count);
typedef MmsResult (MMS_CALL *SubscriptionProbeQueryFn)(SubscriptionRefreshProbeState*);
typedef MmsResult (MMS_CALL *SubscriptionProbeSimpleFn)(void);
