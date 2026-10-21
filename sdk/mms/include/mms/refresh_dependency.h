#ifndef MMS_REFRESH_DEPENDENCY_H
#define MMS_REFRESH_DEPENDENCY_H
#include "refresh.h"
#pragma pack(push, 8)

typedef struct MmsUiRefreshDependencyCapabilitiesV1 {
	MmsHeader h;
	uint32_t maxDependenciesPerPlugin, maxDependentsPerRequest;
	uint32_t flags, reserved;
} MmsUiRefreshDependencyCapabilitiesV1;

/* Both endpoints are the existing published UI target identities owned by
   this exact plugin client. Geometry changes keep an edge; instance/project
   changes invalidate it. flags and reserved must be zero. */
typedef struct MmsUiRefreshDependencyDescV1 {
	MmsHeader h;
	MmsUiTargetRefV1 upstream, dependent;
	uint32_t flags, reserved;
} MmsUiRefreshDependencyDescV1;

/* queryService("mms.ui.refresh.dependencies", 1, 0, ...).
   All three functions are UI-thread only. Edges form an explicit acyclic
   graph; they do not subscribe to data-source publications. DEPENDENTS uses
   the original refresh.request/ticket service and visits reachable targets
   in dependency order, excluding the upstream seed and unrelated children.
   Edge changes stale this client's pending dependency requests. Those
   requests accept whole-target damage only and capture DATA versions as one
   cohort. Failed calls leave outputs unchanged. */
typedef struct MmsUiRefreshDependencyApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiRefreshDependencyCapabilitiesV1*);
	MmsResult (MMS_CALL *registerDependency)(void*, const MmsUiRefreshDependencyDescV1*, MmsHandle*);
	MmsResult (MMS_CALL *unregisterDependency)(void*, MmsHandle);
} MmsUiRefreshDependencyApiV1;

#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiRefreshDependencyCapabilitiesV1) == 24, "MMS UI dependency capabilities");
static_assert(sizeof(MmsUiRefreshDependencyDescV1) == 80, "MMS UI dependency descriptor");
static_assert(sizeof(MmsUiRefreshDependencyApiV1) == 40, "MMS UI dependency API");
#endif
#endif
