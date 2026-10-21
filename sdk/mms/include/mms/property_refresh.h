#ifndef MMS_PROPERTY_REFRESH_H
#define MMS_PROPERTY_REFRESH_H
#include "refresh.h"
#define MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY 1u
#define MMS_UI_PROPERTY_REFRESH_WAIT_BINDING 1u
#define MMS_UI_PROPERTY_REFRESH_WAIT_TARGET 2u
#define MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY 4u
#define MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER 8u
#define MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE 16u
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
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiPropertyRefV1) == 32, "MMS property reference");
static_assert(sizeof(MmsUiPropertyRefreshCapabilitiesV1) == 32, "MMS property refresh capabilities");
static_assert(sizeof(MmsUiPropertyRefreshSubscriptionDescV1) == 88, "MMS property refresh descriptor");
static_assert(sizeof(MmsUiPropertyRefreshSubscriptionStateV1) == 120, "MMS property refresh state");
static_assert(sizeof(MmsUiPropertyRefreshApiV1) == 64, "MMS property refresh API");
#endif
#endif
