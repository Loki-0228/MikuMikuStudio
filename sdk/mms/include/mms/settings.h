#ifndef MMS_SETTINGS_H
#define MMS_SETTINGS_H
#include "plugin.h"

#define MMS_SETTINGS_MAX_JSON_BYTES (64u * 1024u)
#pragma pack(push, 8)
/* Optional service "mms.settings" 1.0, manifest capability "settings.global".
   The context selects the exact owning manifest ID; it is not transferable
   to another plugin. All calls are on the host UI thread. No path or host-
   owned pointer is returned. */
typedef struct MmsSettingsApiV1 {
    MmsHeader h;
    void* context;
    /* Own the output storage. UTF-8 JSON object, no terminator. NULL/0
       measures the current object (OK and outBytes). A short non-NULL
       buffer returns MEMORY and sets only outBytes to the required size.
       All other failures, including NOT_FOUND for no object, leave both
       outputs unchanged. A subsequent copy can observe a newer value.
       The buffer and outBytes must not overlap. Allowed in create, start
       and ordinary owner enqueueUi run callbacks. */
    MmsResult (MMS_CALL *read)(void*, char* buffer, uint64_t capacity, uint64_t* outBytes);
    /* Ordinary owner enqueueUi run callbacks only. Pass the previously read
       JSON object as expected, or {NULL, 0} to require absence. Both inputs
       are borrowed UTF-8 bytes for this call. A stale baseline returns
       CONFLICT; unrelated settings changes do not cause a conflict. Omitted
       object keys are preserved, null is literal, and arrays replace as a
       unit. Object members cannot be replaced with non-objects. No delete. */
    MmsResult (MMS_CALL *compareAndMerge)(void*, MmsString expected, MmsString patch);
} MmsSettingsApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsSettingsApiV1) == 32, "MMS global settings layout");
#endif
#endif
