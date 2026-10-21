#ifndef MMS_PROPERTY_REFRESH_H
#define MMS_PROPERTY_REFRESH_H
#include "refresh.h"
#define MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY 1u
#define MMS_UI_PROPERTY_REFRESH_WAIT_BINDING 1u
#define MMS_UI_PROPERTY_REFRESH_WAIT_TARGET 2u
#define MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY 4u
#define MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER 8u
#define MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE 16u
#define MMS_UI_PROPERTY_REFRESH_WAIT_GRAPH 32u
#define MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE 64u
#define MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY 128u
#pragma pack(push, 8)
typedef struct MmsUiPropertyRefV1 {
    MmsHeader h;
    MmsHandle property;
    uint64_t registrationGeneration, projectEpoch;
} MmsUiPropertyRefV1;
typedef struct MmsUiPropertyRefreshCapabilitiesV1 {
    MmsHeader h;
    uint32_t maxSubscriptionsPerPlugin, supportedKinds, supportedFlags, flags;
    uint64_t reserved;
} MmsUiPropertyRefreshCapabilitiesV1;
typedef struct MmsUiPropertyRefreshSubscriptionDescV1 {
    MmsHeader h;
    MmsUiTargetRefV1 target;
    MmsUiPropertyRefV1 property;
    uint32_t kinds, flags;
    uint64_t reserved;
} MmsUiPropertyRefreshSubscriptionDescV1;
typedef struct MmsUiPropertyRefreshSubscriptionStateV1 {
    MmsHeader h;
    MmsHandle subscription;
    MmsUiTargetRefV1 target;
    MmsUiPropertyRefV1 property;
    uint64_t latestSequence, submittedSequence, completedSequence;
    MmsResult lastResult;
    uint32_t pending, waitingFor, reserved;
} MmsUiPropertyRefreshSubscriptionStateV1;
/* queryService("mms.ui.refresh.property", 1, 0, ...).
   Capabilities, references, subscribe and unsubscribe require the UI thread.
   notify and query permit workers. References name original properties and
   original INPUT presentations; no extra source, target or UI object is made.
   A sequence coalesces change notifications, not immutable property values.
   DATA rereads the original property at the UI safe point. Explicit DATA
   bindings take priority; the subscription resumes after their removal.
   Read-only refresh callbacks cannot notify or mutate subscriptions. Ordinary
   commands may notify. FOLLOW_HISTORY is available only when advertised.
   Failed calls leave caller outputs unchanged. Retired identities are STALE. */
typedef struct MmsUiPropertyRefreshApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiPropertyRefreshCapabilitiesV1*);
    MmsResult (MMS_CALL *getPropertyRef)(void*, MmsHandle, MmsUiPropertyRefV1*);
    MmsResult (MMS_CALL *subscribe)(void*, const MmsUiPropertyRefreshSubscriptionDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *notify)(void*, const MmsUiPropertyRefV1*);
    MmsResult (MMS_CALL *query)(void*, MmsHandle, MmsUiPropertyRefreshSubscriptionStateV1*);
    MmsResult (MMS_CALL *unsubscribe)(void*, MmsHandle);
} MmsUiPropertyRefreshApiV1;
typedef struct MmsUiPropertyRefreshScopedCapabilitiesV1 {
    MmsHeader h;
    uint32_t maxSubscriptionsPerPlugin, supportedKinds, supportedFlags, flags;
    uint64_t reserved;
    uint32_t supportedScopes, reservedTail;
} MmsUiPropertyRefreshScopedCapabilitiesV1;
typedef struct MmsUiPropertyRefreshScopedSubscriptionDescV1 {
    MmsHeader h;
    MmsUiPropertyRefV1 property;
    MmsUiTargetRefV1 seed;
    uint32_t scope, kinds, flags, reserved;
    uint64_t reservedTail;
} MmsUiPropertyRefreshScopedSubscriptionDescV1;
typedef struct MmsUiPropertyRefreshScopedSubscriptionStateV1 {
    MmsHeader h;
    MmsHandle subscription;
    MmsUiTargetRefV1 target;
    MmsUiPropertyRefV1 property;
    uint64_t latestSequence, submittedSequence, completedSequence;
    MmsResult lastResult;
    uint32_t pending, waitingFor, reserved;
    uint32_t scope, kinds, flags, reservedTail0;
    uint64_t workGeneration, submittedWorkGeneration, completedWorkGeneration, submittedDependencyRevision, reservedTail;
} MmsUiPropertyRefreshScopedSubscriptionStateV1;
/* queryService("mms.ui.refresh.property", 1, 1, ...). The 1.0 API prefix and
   all 1.0 callback structures retain their original layouts and behavior.
   Property notifications may drive an independent original seed. SUBTREE
   and DEPENDENTS use one original composite refresh ticket; DEPENDENTS always
   excludes the seed. Each member prefers its explicit DATA binding, otherwise
   an original native INPUT is reread at consumption. Notification/history
   sequences are not immutable property snapshots. Accepted explicit-source
   cohorts survive later publications; relevant graph/binding changes may
   invalidate them. workGeneration distinguishes retries at one sequence.
   SELF delegates to 1.0 and requires the INPUT's original property. New and
   old query/unsubscribe callbacks share subscription handles. */
typedef struct MmsUiPropertyRefreshApiV1_1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiPropertyRefreshCapabilitiesV1*);
    MmsResult (MMS_CALL *getPropertyRef)(void*, MmsHandle, MmsUiPropertyRefV1*);
    MmsResult (MMS_CALL *subscribe)(void*, const MmsUiPropertyRefreshSubscriptionDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *notify)(void*, const MmsUiPropertyRefV1*);
    MmsResult (MMS_CALL *query)(void*, MmsHandle, MmsUiPropertyRefreshSubscriptionStateV1*);
    MmsResult (MMS_CALL *unsubscribe)(void*, MmsHandle);
    MmsResult (MMS_CALL *getScopedCapabilities)(void*, MmsUiPropertyRefreshScopedCapabilitiesV1*);
    MmsResult (MMS_CALL *subscribeScoped)(void*, const MmsUiPropertyRefreshScopedSubscriptionDescV1*, MmsHandle*);
    MmsResult (MMS_CALL *queryScoped)(void*, MmsHandle, MmsUiPropertyRefreshScopedSubscriptionStateV1*);
} MmsUiPropertyRefreshApiV1_1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiPropertyRefV1) == 32, "MMS property reference");
static_assert(sizeof(MmsUiPropertyRefreshCapabilitiesV1) == 32, "MMS property refresh capabilities");
static_assert(sizeof(MmsUiPropertyRefreshSubscriptionDescV1) == 88, "MMS property refresh descriptor");
static_assert(sizeof(MmsUiPropertyRefreshSubscriptionStateV1) == 120, "MMS property refresh state");
static_assert(sizeof(MmsUiPropertyRefreshApiV1) == 64, "MMS property refresh API");
static_assert(sizeof(MmsUiPropertyRefreshScopedCapabilitiesV1) == 40, "MMS scoped property refresh capabilities");
static_assert(sizeof(MmsUiPropertyRefreshScopedSubscriptionDescV1) == 96, "MMS scoped property refresh descriptor");
static_assert(sizeof(MmsUiPropertyRefreshScopedSubscriptionStateV1) == 176, "MMS scoped property refresh state");
static_assert(sizeof(MmsUiPropertyRefreshApiV1_1) == 88, "MMS scoped property refresh API");
#endif
#endif
