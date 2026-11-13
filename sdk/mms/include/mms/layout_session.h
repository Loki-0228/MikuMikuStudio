#ifndef MMS_LAYOUT_SESSION_H
#define MMS_LAYOUT_SESSION_H
#include "layout.h"

/* mms.ui.layout.session 1.0/1.1. All calls require the UI thread. Mutations
   require a safe point outside callbacks. Handles share the existing UI
   registry identity; no HWND or second layout tree is exposed. */
#define MMS_LAYOUT_SESSION_VISIBLE 1u
#define MMS_LAYOUT_SESSION_SIZE 2u
#define MMS_LAYOUT_SESSION_ORDER 4u
#define MMS_LAYOUT_SESSION_PARENT 8u
#define MMS_LAYOUT_SESSION_TOOLBAR 1u
#define MMS_LAYOUT_SESSION_PROPERTIES 2u
#define MMS_LAYOUT_SESSION_LEFT 4u
#define MMS_LAYOUT_SESSION_RIGHT 8u
/* Service 1.1 adds own PANELs directly under mms.workspace.tabs.extensions.
   Service 1.0 still reports mounts=15 and cannot query/begin PANEL targets. */
#define MMS_LAYOUT_SESSION_WORKSPACE 16u
#define MMS_LAYOUT_SESSION_ACTIVE 0u
#define MMS_LAYOUT_SESSION_ROLLED_BACK 1u
#define MMS_LAYOUT_SESSION_COMMITTED 2u
#define MMS_LAYOUT_SESSION_REVOKED 3u
/* Cumulative reasons: a partial takeover leaves the session active. */
#define MMS_LAYOUT_SESSION_PERSISTENT_EDIT 1u
#define MMS_LAYOUT_SESSION_TARGET_CHANGED 2u
#define MMS_LAYOUT_SESSION_HOST_INTERACTION 4u
#define MMS_LAYOUT_SESSION_STOPPING 8u
#define MMS_LAYOUT_SESSION_REVISION_EXHAUSTED 16u
#pragma pack(push, 8)
typedef struct MmsUiLayoutSessionCapsV1 {
    MmsHeader h;
    uint32_t fields, mounts, maxTargets, maxSessionsPerPlugin, maxSessionsGlobal, reserved;
} MmsUiLayoutSessionCapsV1;
typedef struct MmsUiLayoutSessionTargetInfoV1 {
    MmsHeader h;
    MmsHandle element;
    uint32_t fields, mount;
} MmsUiLayoutSessionTargetInfoV1;
typedef struct MmsUiLayoutSessionTargetV1 {
    MmsHeader h;
    MmsHandle element;
    uint32_t fields, reserved;
} MmsUiLayoutSessionTargetV1;
typedef struct MmsUiLayoutSessionValueV1 {
    MmsHeader h;
    MmsHandle element;
    uint32_t fields, visible;
    float width, height;
    int32_t order;
    uint32_t reserved;
} MmsUiLayoutSessionValueV1;
typedef struct MmsUiLayoutSessionInfoV1 {
    MmsHeader h;
    uint64_t revision;
    uint32_t state, reasons, targetCount, activeTargetCount;
} MmsUiLayoutSessionInfoV1;
typedef struct MmsUiLayoutSessionApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *queryCapabilities)(void*, MmsUiLayoutSessionCapsV1*);
    MmsResult (MMS_CALL *queryTarget)(void*, MmsHandle, MmsUiLayoutSessionTargetInfoV1*);
    /* targets: 1..maxTargets, recordVersion=1, exact stride, distinct handles.
       begin only claims fields. Input memory is copied before return. */
    MmsResult (MMS_CALL *begin)(void*, MmsSpan targets, MmsHandle*);
    /* Full set of remaining fields, in original target order. Fields absent
       from a value must be zero (including reserved). visible is 0 or 1. */
    MmsResult (MMS_CALL *apply)(void*, MmsHandle, uint64_t revision, MmsSpan values);
    MmsResult (MMS_CALL *query)(void*, MmsHandle, MmsUiLayoutSessionInfoV1*);
    /* Original begin index, including targets with no remaining fields.
       Terminal sessions return fields=0. Stale revision leaves output intact. */
    MmsResult (MMS_CALL *queryClaim)(void*, MmsHandle, uint64_t revision, uint32_t index, MmsUiLayoutSessionTargetV1*);
    MmsResult (MMS_CALL *rollback)(void*, MmsHandle, uint64_t revision);
    MmsResult (MMS_CALL *commit)(void*, MmsHandle, uint64_t revision);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
} MmsUiLayoutSessionApiV1;
/* 1.2 records use h={sizeof(record),1,2} and span.recordVersion=2.
   Parent is a registered target handle, never an HWND. Parent claims require
   a nonzero declared destination at begin; begin does not change the tree.
   Only own native controls/groups within one native mount can change parent.
   Unused parent and all legacy reserved/unused fields must remain zero. */
typedef struct MmsUiLayoutSessionTargetV1_2 {
    MmsHeader h;
    MmsHandle element;
    uint32_t fields, reserved;
    MmsHandle parent;
} MmsUiLayoutSessionTargetV1_2;
typedef struct MmsUiLayoutSessionValueV1_2 {
    MmsHeader h;
    MmsHandle element;
    uint32_t fields, visible;
    float width, height;
    int32_t order;
    uint32_t reserved;
    MmsHandle parent;
} MmsUiLayoutSessionValueV1_2;
typedef struct MmsUiLayoutSessionApiV1_2 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *queryCapabilities)(void*, MmsUiLayoutSessionCapsV1*);
    MmsResult (MMS_CALL *queryTarget)(void*, MmsHandle, MmsUiLayoutSessionTargetInfoV1*);
    MmsResult (MMS_CALL *begin)(void*, MmsSpan, MmsHandle*);
    MmsResult (MMS_CALL *apply)(void*, MmsHandle, uint64_t, MmsSpan);
    MmsResult (MMS_CALL *query)(void*, MmsHandle, MmsUiLayoutSessionInfoV1*);
    MmsResult (MMS_CALL *queryClaim)(void*, MmsHandle, uint64_t, uint32_t, MmsUiLayoutSessionTargetV1*);
    MmsResult (MMS_CALL *rollback)(void*, MmsHandle, uint64_t);
    MmsResult (MMS_CALL *commit)(void*, MmsHandle, uint64_t);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    MmsResult (MMS_CALL *queryCapabilitiesEx)(void*, MmsUiLayoutSessionCapsV1*);
    MmsResult (MMS_CALL *queryTargetEx)(void*, MmsHandle, MmsUiLayoutSessionTargetInfoV1*);
    MmsResult (MMS_CALL *beginEx)(void*, MmsSpan, MmsHandle*);
    MmsResult (MMS_CALL *applyEx)(void*, MmsHandle, uint64_t, MmsSpan);
    MmsResult (MMS_CALL *queryClaimEx)(void*, MmsHandle, uint64_t, uint32_t, MmsUiLayoutSessionTargetV1_2*);
} MmsUiLayoutSessionApiV1_2;
#pragma pack(pop)
/* The service table retains its size and nine function pointers in 1.1;
   its h.minor is the requested service minor (0 or 1). All input/output
   records remain major=1, minor=0. Sessions share the same handles across
   both tables: apply/query/queryClaim/rollback/commit/release on an existing
   handle do not create another session or change its target identity.
   Output records must be initialized with size, major=1, minor=0.
   Failed calls preserve outputs. Stop permits queries, rollback and release.
   Active release is BUSY; explicit rollback/commit precedes release.
   No destructor/thread-independent lifetime is implied by a service table. */
/* 1.2's old prefix still accepts only fields 1|2|4 and uses 1.0 records.
   Ex capabilities advertise fields=15,mounts=31; PANEL targets never advertise
   Parent. Ex caps/targetInfo and query info keep their existing 1.0 headers.
   Ex and prefix operations share session handles, revisions and history.
   queryClaimEx returns the declared/applied parent, or zero if Parent is no
   longer claimed; terminal claims have fields=parent=0. Old queryClaim returns
   UNSUPPORTED for active Parent claims without touching its output. */
#ifdef __cplusplus
static_assert(sizeof(MmsUiLayoutSessionCapsV1)==32, "layout session caps");
static_assert(sizeof(MmsUiLayoutSessionTargetInfoV1)==24, "layout session target info");
static_assert(sizeof(MmsUiLayoutSessionTargetV1)==24, "layout session target");
static_assert(sizeof(MmsUiLayoutSessionValueV1)==40, "layout session value");
static_assert(sizeof(MmsUiLayoutSessionInfoV1)==32, "layout session info");
static_assert(sizeof(MmsUiLayoutSessionApiV1)==88, "layout session service");
static_assert(sizeof(MmsUiLayoutSessionTargetV1_2)==32, "layout session 1.2 target");
static_assert(sizeof(MmsUiLayoutSessionValueV1_2)==48, "layout session 1.2 value");
static_assert(sizeof(MmsUiLayoutSessionApiV1_2)==128, "layout session 1.2 service");
static_assert(offsetof(MmsUiLayoutSessionApiV1_2, queryCapabilitiesEx)==sizeof(MmsUiLayoutSessionApiV1), "layout session prefix");
#endif
#endif
