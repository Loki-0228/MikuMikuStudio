#ifndef MMS_REFRESH_H
#define MMS_REFRESH_H
#include "ui.h"
#define MMS_UI_REFRESH_REPAINT 1u
#define MMS_UI_REFRESH_DATA 2u
#define MMS_UI_REFRESH_STATE 4u
#define MMS_UI_REFRESH_LAYOUT 8u
#define MMS_UI_REFRESH_SELF 1u
/* Original registered subtree, including a drawable root. GROUP exposes only
   SUBTREE. This does not enumerate arbitrary native child windows. */
#define MMS_UI_REFRESH_SUBTREE 2u
#define MMS_UI_REFRESH_DEPENDENTS 4u
#define MMS_UI_REFRESH_PENDING 1u
#define MMS_UI_REFRESH_RUNNING 2u
#define MMS_UI_REFRESH_COMPLETED 3u
#define MMS_UI_REFRESH_CANCELLED 4u
#define MMS_UI_REFRESH_FAILED 5u
#pragma pack(push, 8)
/* element is the ORIGINAL mms.ui registration handle. Instance and project
   tokens are obtained on the UI thread, never fabricated from a layout revision. */
typedef struct MmsUiTargetRefV1 {
	MmsHeader h;
	MmsHandle element;
	uint64_t instanceGeneration, projectEpoch;
} MmsUiTargetRefV1;
typedef struct MmsUiDirtyRectV1 { float x, y, width, height; } MmsUiDirtyRectV1;
typedef struct MmsUiRefreshCapabilitiesV1 {
	MmsHeader h;
	uint32_t supportedKinds, supportedScopes, maxTicketsPerPlugin, maxRectsPerRequest;
	uint64_t maxPendingBytesPerPlugin;
	uint32_t maxContributionsPerDrain, reserved;
} MmsUiRefreshCapabilitiesV1;
typedef struct MmsUiRefreshTargetV1 {
	MmsHeader h;
	MmsUiTargetRefV1 target;
	uint64_t geometryRevision;
	uint32_t supportedKinds, supportedScopes, ready, visible;
	float width, height;
} MmsUiRefreshTargetV1;
typedef struct MmsUiRefreshRequestV1 {
	MmsHeader h;
	MmsUiTargetRefV1 target;
    /* SELF+DATA: explicit binding and exact sourceRevision. SUBTREE+DATA:
       both zero; atomically capture every bound member's current snapshot. */
	MmsHandle binding;
	uint32_t kinds, scope;
	uint64_t sourceRevision, expectedGeometryRevision;
    /* With REPAINT, local DIP rectangles are relative to the root client area
       (GROUP: retained logical bounds). A nonempty span requires the root's
       exact expectedGeometryRevision. SUBTREE clips/maps to each registered
       drawable member; native child windows are not enumerated. Geometry
       changes before its REPAINT stage fail STALE instead of repainting all.
       count == 0 requests each applicable target's whole client area. */
	MmsSpan dirtyRects;
	uint64_t coalesceKey;
	uint32_t flags, reserved;
} MmsUiRefreshRequestV1;
typedef struct MmsUiRefreshStateV1 {
	MmsHeader h;
	uint32_t status, completedKinds;
	MmsResult result;
	uint32_t reserved;
    /* SUBTREE's sourceRevision is zero for its version set, including after
       DATA succeeds. completedKinds, not this scalar, indicates completion. */
	uint64_t sourceRevision, geometryRevision;
} MmsUiRefreshStateV1;
typedef struct MmsUiRefreshApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiRefreshCapabilitiesV1*);
	MmsResult (MMS_CALL *getTarget)(void*, MmsHandle, MmsUiRefreshTargetV1*);
	MmsResult (MMS_CALL *request)(void*, const MmsUiRefreshRequestV1*, MmsHandle*);
	MmsResult (MMS_CALL *query)(void*, MmsHandle, MmsUiRefreshStateV1*);
	MmsResult (MMS_CALL *cancel)(void*, MmsHandle);
	MmsResult (MMS_CALL *release)(void*, MmsHandle);
} MmsUiRefreshApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiTargetRefV1) == 32, "MMS refresh target reference");
static_assert(sizeof(MmsUiDirtyRectV1) == 16, "MMS dirty rectangle");
static_assert(sizeof(MmsUiRefreshCapabilitiesV1) == 40, "MMS refresh capabilities");
static_assert(sizeof(MmsUiRefreshTargetV1) == 72, "MMS refresh target");
static_assert(sizeof(MmsUiRefreshRequestV1) == 112, "MMS refresh request");
static_assert(sizeof(MmsUiRefreshStateV1) == 40, "MMS refresh state");
static_assert(sizeof(MmsUiRefreshApiV1) == 64, "MMS refresh API");
#endif
#endif
