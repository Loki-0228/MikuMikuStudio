#ifndef MMS_PROPOSAL_LAYOUT_H
#define MMS_PROPOSAL_LAYOUT_H
#include "ui.h"
#define MMS_UI_LAYOUT 64u
#define MMS_FLOW_AUTO 0u
#define MMS_FLOW_ROW 1u
#define MMS_FLOW_COLUMN 2u
#define MMS_FLOW_WRAP 3u
#define MMS_FLOW_GRID 4u
#define MMS_HINT_KEEP_TOGETHER 1u
#define MMS_HINT_PRESERVE_HIDDEN_SLOT 2u
#define MMS_HINT_EXPLICIT_SPACING 4u
/* Opt in to a command menu when toolbar space is insufficient. The existing
   overflow field and object identity are used; panels and menus do not opt in. */
#define MMS_UI_OVERFLOW_MENU 3u
#pragma pack(push, 8)
typedef struct MmsUiLayoutHintsV1 {
	MmsHeader h;
	uint32_t flow, columns;
	int32_t order;
	uint32_t flags;
	float minimum[2], preferred[2], maximum[2];
	float grow[2], shrink[2];
	float gap[2], padding[4];
	uint32_t align, overflow;
} MmsUiLayoutHintsV1;
typedef struct MmsUiLayoutItemV1 {
	MmsHeader h;
	MmsString elementId;
	uint64_t generation;
	MmsUiLayoutHintsV1 defaults, effective;
	float rect[4];
	uint32_t visible, overflowed;
} MmsUiLayoutItemV1;
typedef struct MmsUiLayoutSnapshotV1 {
	MmsHeader h;
	MmsHandle handle;
	uint64_t uiRevision, geometryRevision;
	MmsSpan items;
} MmsUiLayoutSnapshotV1;
typedef struct MmsUiLayoutApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *setDefaultHints)(void*, MmsString, const MmsUiLayoutHintsV1*);
	MmsResult (MMS_CALL *acquireSnapshot)(void*, MmsUiLayoutSnapshotV1*);
	void (MMS_CALL *releaseSnapshot)(void*, MmsHandle);
	MmsResult (MMS_CALL *stageHints)(void*, MmsHandle, MmsString, uint64_t, const MmsUiLayoutHintsV1*);
} MmsUiLayoutApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiLayoutHintsV1) == 96, "MMS layout hints");
static_assert(sizeof(MmsUiLayoutItemV1) == 248, "MMS layout item");
static_assert(sizeof(MmsUiLayoutSnapshotV1) == 56, "MMS layout snapshot");
static_assert(sizeof(MmsUiLayoutApiV1) == 48, "MMS layout service");
#endif
#endif
