#ifndef MMS_REFRESH_STATE_H
#define MMS_REFRESH_STATE_H
#include "refresh.h"
#define MMS_UI_STATE_LABEL 1u
#define MMS_UI_STATE_DESCRIPTION 2u
#define MMS_UI_STATE_DISABLED_REASON 4u
#define MMS_UI_STATE_ENABLED 8u
#define MMS_UI_STATE_READ_ONLY 16u
#define MMS_UI_STATE_VISIBLE 32u
#pragma pack(push, 8)
/* All text is UTF-8, without embedded NUL. A set field may be empty.
   Unset fields use the host's registered metadata/state baseline.
   writePrepared copies the bytes before returning; no plugin string is retained. */
typedef struct MmsUiStateContentV1 {
	MmsHeader h;
	uint32_t fields, reserved;
	MmsString label, description, disabledReason;
} MmsUiStateContentV1;

/* INPUT-only scalar extension of the unchanged 64-byte prefix. Request each
   scalar explicitly in content.fields and set content.h.size to this size.
   New scalar bits require the complete tail. Values are 0 or 1; unset values
   and both reserved fields must be zero. The provider can only narrow host
   availability: enabled/visible are ANDed with the host baseline, readOnly is
   ORed with it. No scalar can grant editing permission or reveal hidden UI.
   Query supportedFields before writing. Uses the original 1.0 service,
   provider, callback, builder and STATE request; no extra registration. */
typedef struct MmsUiInputStateContentV1 {
	MmsUiStateContentV1 content;
	uint32_t enabled, readOnly, visible, reserved;
} MmsUiInputStateContentV1;
typedef MmsResult (MMS_CALL *MmsUiStateContentCallbackV1)(void*, const MmsUiTargetRefV1*, MmsHandle builder);
typedef struct MmsUiStateContentDescV1 {
	MmsHeader h;
	MmsUiTargetRefV1 target;
	void* context;
	MmsUiStateContentCallbackV1 prepare;
	uint32_t fields, reserved;
} MmsUiStateContentDescV1;
typedef struct MmsUiStateContentCapabilitiesV1 {
	MmsHeader h;
	uint32_t supportedFields, maxProvidersPerPlugin;
	uint64_t maxBytesPerField, maxRetainedBytesPerPlugin, maxRetainedBytesGlobal;
} MmsUiStateContentCapabilitiesV1;
/* mms.ui.refresh.state 1.0. All calls are UI-thread only. Registration does
   not refresh. STATE uses the original refresh request, queue and ticket.
   update/unregister invalidate older pending STATE contributions only.
   A provider must write once and return promptly; the builder expires on return. */
typedef struct MmsUiRefreshStateContentApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiStateContentCapabilitiesV1*);
	MmsResult (MMS_CALL *registerProvider)(void*, const MmsUiStateContentDescV1*, MmsHandle*);
	MmsResult (MMS_CALL *updateProvider)(void*, MmsHandle, const MmsUiStateContentDescV1*);
	MmsResult (MMS_CALL *unregisterProvider)(void*, MmsHandle);
	MmsResult (MMS_CALL *writePrepared)(void*, MmsHandle builder, const MmsUiStateContentV1*);
} MmsUiRefreshStateContentApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiStateContentV1) == 64, "MMS state content");
static_assert(sizeof(MmsUiInputStateContentV1) == 80, "MMS input state content");
static_assert(offsetof(MmsUiInputStateContentV1, content) == 0, "MMS input state prefix");
static_assert(offsetof(MmsUiInputStateContentV1, enabled) == 64, "MMS input state enabled");
static_assert(offsetof(MmsUiInputStateContentV1, readOnly) == 68, "MMS input state readonly");
static_assert(offsetof(MmsUiInputStateContentV1, visible) == 72, "MMS input state visible");
static_assert(offsetof(MmsUiInputStateContentV1, reserved) == 76, "MMS input state reserved");
static_assert(sizeof(MmsUiStateContentDescV1) == 64, "MMS state content provider");
static_assert(sizeof(MmsUiStateContentCapabilitiesV1) == 40, "MMS state content capabilities");
static_assert(sizeof(MmsUiRefreshStateContentApiV1) == 56, "MMS state content API");
#endif
#endif
