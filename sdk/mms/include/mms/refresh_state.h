#ifndef MMS_REFRESH_STATE_H
#define MMS_REFRESH_STATE_H
#include "refresh.h"
#define MMS_UI_STATE_LABEL 1u
#define MMS_UI_STATE_DESCRIPTION 2u
#define MMS_UI_STATE_DISABLED_REASON 4u
#pragma pack(push, 8)
/* All text is UTF-8, without embedded NUL. A set field may be empty.
   Unset fields use the registered label/description or command disabledReason.
   writePrepared copies the bytes before returning; no plugin string is retained. */
typedef struct MmsUiStateContentV1 {
	MmsHeader h;
	uint32_t fields, reserved;
	MmsString label, description, disabledReason;
} MmsUiStateContentV1;
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
static_assert(sizeof(MmsUiStateContentDescV1) == 64, "MMS state content provider");
static_assert(sizeof(MmsUiStateContentCapabilitiesV1) == 40, "MMS state content capabilities");
static_assert(sizeof(MmsUiRefreshStateContentApiV1) == 56, "MMS state content API");
#endif
#endif
