#ifndef MMS_LEGACY_PLAYBACK_MAP_H
#define MMS_LEGACY_PLAYBACK_MAP_H

#include "plugin.h"

#define MMS_LEGACY_PLAYBACK_MAP_EXPORT "MMSAcquireLegacyPlaybackMap"
#define MMS_LEGACY_PLAYBACK_MAP_VERSION_1 1u
#define MMS_LEGACY_PLAYBACK_MAP_PLAYING 1u

#pragma pack(push, 8)
typedef struct MmsLegacyPlaybackMapV1 {
    uint32_t byteSize;
    uint32_t version;
    uint32_t flags;
    int32_t timelineFrame;
    uint64_t snapshotId;
    double timelineSeconds;
} MmsLegacyPlaybackMapV1;
#pragma pack(pop)

/* Resolve the named export from the host executable. Call on its UI thread.
   On success the returned Windows HANDLE maps with FILE_MAP_READ and must be
   closed by the caller. NULL means failure (see GetLastError). The mapped
   snapshot remains valid independently of newer snapshots and host state. */
typedef void* (MMS_CALL *MmsAcquireLegacyPlaybackMapFn)(uint32_t version);

#endif
