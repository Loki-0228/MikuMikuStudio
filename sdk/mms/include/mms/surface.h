#ifndef MMS_PROPOSAL_SURFACE_H
#define MMS_PROPOSAL_SURFACE_H
#include "ui.h"
#define MMS_SURFACE_MULTIPLE_VIEWS 1u
#define MMS_ANCHOR_NATIVE_CHILD 1u
#define MMS_ANCHOR_DOCK 2u
#pragma pack(push, 8)
typedef struct MmsUiAnchorV1 {
	MmsHeader h;
	MmsString id, label;
	uint64_t allowedKinds;
	uint32_t flags, reserved;
} MmsUiAnchorV1;
typedef struct MmsUiRectV1 { int32_t x, y; uint32_t width, height; } MmsUiRectV1;
typedef struct MmsUiMountV1 {
	MmsHeader h;
	MmsString elementId;
	uintptr_t parentWindow;
	MmsUiRectV1 bounds;
	uint32_t dpi, reserved;
} MmsUiMountV1;
typedef struct MmsUiSurfaceDescV1 {
	MmsHeader h;
	MmsString id;
	void* context;
	uint32_t flags, reserved;
	float minimumWidth, minimumHeight, preferredWidth, preferredHeight;
	MmsResult (MMS_CALL *createView)(void*, const MmsUiMountV1*, void**, uintptr_t*);
	void (MMS_CALL *resizeView)(void*, const MmsUiRectV1*, uint32_t dpi);
	void (MMS_CALL *showView)(void*, uint32_t visible);
	MmsResult (MMS_CALL *focusView)(void*);
	void (MMS_CALL *destroyView)(void*);
} MmsUiSurfaceDescV1;
typedef struct MmsUiSurfaceApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *acquireAnchors)(void*, MmsSpan*, MmsHandle*);
	void (MMS_CALL *releaseAnchors)(void*, MmsHandle);
	MmsResult (MMS_CALL *registerSurface)(void*, const MmsUiSurfaceDescV1*, MmsHandle*);
	MmsResult (MMS_CALL *unregisterSurface)(void*, MmsHandle);
} MmsUiSurfaceApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiAnchorV1) == 56, "MMS anchor layout");
static_assert(sizeof(MmsUiMountV1) == 56, "MMS mount layout");
static_assert(sizeof(MmsUiSurfaceDescV1) == 96, "MMS surface factory layout");
static_assert(sizeof(MmsUiSurfaceApiV1) == 48, "MMS surface API layout");
#endif
#endif
