#ifndef MMS_STORAGE_H
#define MMS_STORAGE_H
#include "plugin.h"
#pragma pack(push, 8)
/* Register with host.registerProvider("mms.serializer", own pluginId, ...).
   UI thread only. Schema 0 + empty bytes means a new/absent project state.
   prepareLoad must not publish changes. A successful candidate is consumed
   exactly once by commitLoad (no allocation/failure) or cancelLoad.
   save bytes remain valid until releaseBytes; the host copies before release. */
typedef struct MmsSerializerV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *prepareLoad)(void*, MmsBytes, uint32_t, MmsHandle*);
    void (MMS_CALL *commitLoad)(void*, MmsHandle);
    void (MMS_CALL *cancelLoad)(void*, MmsHandle);
    MmsResult (MMS_CALL *save)(void*, MmsBytes*, uint32_t*, MmsHandle*);
    void (MMS_CALL *releaseBytes)(void*, MmsHandle);
} MmsSerializerV1;
typedef struct MmsStorageApiV1 {
    MmsHeader h;
    void* context;
    /* Last successfully loaded/published bytes for this plugin, not live state.
       Empty bytes are valid. No record returns NOT_FOUND without output writes.
       Reads/releases use the UI thread; leases survive project replacement. */
    MmsResult (MMS_CALL *readSaved)(void*, MmsBytes*, uint32_t*, MmsHandle*);
    void (MMS_CALL *releaseSaved)(void*, MmsHandle);
} MmsStorageApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsSerializerV1) == 56, "MMS serializer layout");
static_assert(sizeof(MmsStorageApiV1) == 32, "MMS storage layout");
#endif
#endif
