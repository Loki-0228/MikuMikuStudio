#ifndef MMS_REFRESH_SUBSCRIPTION_H
#define MMS_REFRESH_SUBSCRIPTION_H
#include "refresh_binding.h"
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
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiRefreshSubscriptionCapabilitiesV1) == 32, "MMS refresh subscription capabilities");
static_assert(sizeof(MmsUiRefreshSubscriptionDescV1) == 32, "MMS refresh subscription descriptor");
static_assert(sizeof(MmsUiRefreshSubscriptionStateV1) == 72, "MMS refresh subscription state");
static_assert(sizeof(MmsUiRefreshSubscriptionApiV1) == 48, "MMS refresh subscription API");
#endif
#endif
