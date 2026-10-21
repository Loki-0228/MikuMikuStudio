#ifndef MMS_REFRESH_SUBSCRIPTION_H
#define MMS_REFRESH_SUBSCRIPTION_H
#include "refresh_binding.h"
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH 1u
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET 2u
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY 4u
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE 8u
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER 16u
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_BINDING 32u
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE 64u
#define MMS_UI_REFRESH_SUBSCRIPTION_WAIT_DISPLAY_CAPACITY 128u
#pragma pack(push, 8)
typedef struct MmsUiRefreshSubscriptionCapabilitiesV1 {
    MmsHeader h;
    uint32_t maxSubscriptionsPerPlugin, supportedKinds, supportedScopes, flags;
    uint32_t reserved[2];
} MmsUiRefreshSubscriptionCapabilitiesV1;
typedef struct MmsUiRefreshSubscriptionDescV1 {
    MmsHeader h;
    MmsHandle binding;
    uint32_t kinds, flags;
    uint64_t reserved;
} MmsUiRefreshSubscriptionDescV1;
typedef struct MmsUiRefreshSubscriptionStateV1 {
    MmsHeader h;
    MmsHandle subscription, binding;
    uint64_t projectEpoch, latestRevision, submittedRevision, completedRevision;
    MmsResult lastResult;
    uint32_t pending;
    uint64_t reserved;
} MmsUiRefreshSubscriptionStateV1;
/* queryService("mms.ui.refresh.subscribe", 1, 0, ...).
   Registration, capabilities and unsubscription require the UI thread and
   reject read-only callbacks. query permits workers. One subscription refers
   to one existing DATA binding; it does not create a new target or source.
   kinds must include DATA and fit the binding target's current capabilities.
   Publications wake the existing refresh queue without invoking UI callbacks
   on the publishing thread. Accepted tickets retain their exact snapshots;
   publications not yet submitted may coalesce to the newest revision.
   lastResult reports the most recent submission or completion. pending is
   true while a version is waiting for submission or its ticket is unfinished.
   completedRevision advances only on successful completion of all requested
   phases. Invalid or retired handles return STALE without changing output. */
typedef struct MmsUiRefreshSubscriptionApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiRefreshSubscriptionCapabilitiesV1*);
    MmsResult (MMS_CALL *subscribe)(void*, const MmsUiRefreshSubscriptionDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *query)(void*, MmsHandle, MmsUiRefreshSubscriptionStateV1*);
    MmsResult (MMS_CALL *unsubscribe)(void*, MmsHandle);
} MmsUiRefreshSubscriptionApiV1;
typedef struct MmsUiRefreshScopedSubscriptionDescV1 {
    MmsHeader h;
    MmsHandle binding;
    MmsUiTargetRefV1 seed;
    uint32_t scope, kinds, flags, reserved;
    uint64_t reservedTail[2];
} MmsUiRefreshScopedSubscriptionDescV1;
typedef struct MmsUiRefreshScopedSubscriptionStateV1 {
    MmsHeader h;
    MmsHandle subscription, binding;
    uint64_t projectEpoch, latestRevision, submittedRevision, completedRevision;
    MmsResult lastResult;
    uint32_t pending;
    uint64_t reserved;
    MmsUiTargetRefV1 seed;
    uint32_t scope, kinds;
    uint64_t workGeneration, submittedWorkGeneration, completedWorkGeneration, submittedDependencyRevision;
    uint32_t waitingFor, flags;
    uint64_t reservedTail;
} MmsUiRefreshScopedSubscriptionStateV1;
/* queryService("mms.ui.refresh.subscribe", 1, 1, ...). The 1.0 prefix and
   callback structures remain unchanged. The trigger is a real DATA binding;
   seed names the original retained UI object to expand at the UI safe point.
   DEPENDENTS always excludes the seed. The trigger target participates only
   when it is actually reachable. One composite ticket retains the exact trigger
   revision and the simultaneously captured member-source cohort. Pending
   publications may merge; accepted cohorts do not change. workGeneration
   distinguishes graph/binding/source recovery at the same trigger revision.
   Scoped SELF requires seed to be the trigger binding's target. queryScoped
   accepts legacy handles; the legacy query/unsubscribe also accept scoped
   handles. Reserved fields and flags must be zero. */
typedef struct MmsUiRefreshSubscriptionApiV1_1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiRefreshSubscriptionCapabilitiesV1*);
    MmsResult (MMS_CALL *subscribe)(void*, const MmsUiRefreshSubscriptionDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *query)(void*, MmsHandle, MmsUiRefreshSubscriptionStateV1*);
    MmsResult (MMS_CALL *unsubscribe)(void*, MmsHandle);
    MmsResult (MMS_CALL *subscribeScoped)(void*, const MmsUiRefreshScopedSubscriptionDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *queryScoped)(void*, MmsHandle, MmsUiRefreshScopedSubscriptionStateV1*);
} MmsUiRefreshSubscriptionApiV1_1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiRefreshSubscriptionCapabilitiesV1) == 32, "MMS refresh subscription capabilities");
static_assert(sizeof(MmsUiRefreshSubscriptionDescV1) == 32, "MMS refresh subscription descriptor");
static_assert(sizeof(MmsUiRefreshSubscriptionStateV1) == 72, "MMS refresh subscription state");
static_assert(sizeof(MmsUiRefreshSubscriptionApiV1) == 48, "MMS refresh subscription API");
static_assert(sizeof(MmsUiRefreshScopedSubscriptionDescV1) == 80, "MMS scoped refresh subscription descriptor");
static_assert(sizeof(MmsUiRefreshScopedSubscriptionStateV1) == 160, "MMS scoped refresh subscription state");
static_assert(sizeof(MmsUiRefreshSubscriptionApiV1_1) == 64, "MMS scoped refresh subscription API");
#endif
#endif
