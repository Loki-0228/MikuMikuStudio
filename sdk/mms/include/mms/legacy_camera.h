#ifndef MMS_LEGACY_CAMERA_H
#define MMS_LEGACY_CAMERA_H
#include "plugin.h"

#define MMS_LEGACY_CAMERA_API_EXPORT "MMSAcquireLegacyCameraApi"
#define MMS_LEGACY_CAMERA_API_VERSION 1u
#define MMS_LEGACY_CAMERA_EDITABLE 1u

#pragma pack(push, 8)
typedef struct MmsLegacyCameraStateV1 {
    uint32_t byteSize, version;
    uint32_t flags, reserved;
    uint64_t revision;
    float position[3];
    float rotation[3]; /* pitch, yaw, roll, radians */
    float distance; /* signed authored camera distance */
} MmsLegacyCameraStateV1;
typedef struct MmsLegacyCameraApiV1 {
    uint32_t byteSize, version;
    MmsResult (MMS_CALL *read)(MmsLegacyCameraStateV1* outState);
    MmsResult (MMS_CALL *edit)(const MmsLegacyCameraStateV1* expected,
        const MmsLegacyCameraStateV1* desired);
} MmsLegacyCameraApiV1;
#pragma pack(pop)

/* Resolve from GetModuleHandleW(NULL). The table is host-owned and immutable.
   Calls are valid only during a first_person_camera_v4 callback on the UI
   thread. edit atomically adjusts the current authored pose, or leaves it
   unchanged. It does not register a keyframe or add an undo history entry.
   No pointer into MMDMainData or the host scene crosses this boundary. */
typedef const MmsLegacyCameraApiV1* (MMS_CALL *MmsAcquireLegacyCameraApiFn)(uint32_t version);
#endif
