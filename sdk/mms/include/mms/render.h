#pragma once
#include "plugin.h"

// Render service 1.0: device generation, frame information and resource leases
// for plugins that keep device-dependent objects. A lease names plugin-owned
// resources; the host invokes the release callback when the device is lost,
// before the plugin is stopped, or during explicit release. The callback runs
// on the UI thread outside plugin callbacks and must only free device objects.
#define MMS_RENDER_API_VERSION_MAJOR 1u
#define MMS_RENDER_API_VERSION_MINOR 0u
#define MMS_RENDER_LEASE_DEVICE 1u

typedef struct MmsRenderInfoV1 {
    MmsHeader h;
    uint64_t deviceGeneration;
    uint64_t frameIndex;
    uint32_t lost;
    uint32_t reserved;
    uint32_t width;
    uint32_t height;
    uint32_t backBufferFormat;
    uint32_t reserved2;
} MmsRenderInfoV1;

typedef struct MmsRenderLeaseDescV1 {
    MmsHeader h;
    MmsString name;
    void (MMS_CALL *release)(void* context);
    void* context;
    uint32_t flags;
    uint32_t reserved;
} MmsRenderLeaseDescV1;

// Optional per-present observer. It runs on the UI thread from the Present
// path, outside plugin callbacks; it must not call host services or block.
typedef void (MMS_CALL *MmsRenderFrameCallbackV1)(void* context, const MmsRenderInfoV1* info);
typedef struct MmsRenderApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *query)(void* context, MmsRenderInfoV1* out);
    MmsResult (MMS_CALL *acquireLease)(void* context, const MmsRenderLeaseDescV1* desc, MmsHandle* out);
    MmsResult (MMS_CALL *releaseLease)(void* context, MmsHandle lease);
} MmsRenderApiV1;

#define MMS_RENDER_PHASE_BEGIN 1u
#define MMS_RENDER_PHASE_END 2u
// Phase observer for the frame lifecycle. BEGIN runs before scene updates and
// END runs after a successful present. Both run on the UI thread outside plugin
// callbacks and must stay lightweight and read-only.
typedef void (MMS_CALL *MmsRenderPhaseCallbackV1)(void* context, const MmsRenderInfoV1* info, uint32_t phase);
typedef struct MmsRenderApiV1_1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *query)(void* context, MmsRenderInfoV1* out);
    MmsResult (MMS_CALL *acquireLease)(void* context, const MmsRenderLeaseDescV1* desc, MmsHandle* out);
    MmsResult (MMS_CALL *releaseLease)(void* context, MmsHandle lease);
    MmsResult (MMS_CALL *setFrameCallback)(void* context, MmsRenderFrameCallbackV1 callback, void* user);
    void (MMS_CALL *clearFrameCallback)(void* context);
} MmsRenderApiV1_1;

typedef struct MmsRenderApiV1_2 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *query)(void* context, MmsRenderInfoV1* out);
    MmsResult (MMS_CALL *acquireLease)(void* context, const MmsRenderLeaseDescV1* desc, MmsHandle* out);
    MmsResult (MMS_CALL *releaseLease)(void* context, MmsHandle lease);
    MmsResult (MMS_CALL *setFrameCallback)(void* context, MmsRenderFrameCallbackV1 callback, void* user);
    void (MMS_CALL *clearFrameCallback)(void* context);
    MmsResult (MMS_CALL *setPhaseCallback)(void* context, MmsRenderPhaseCallbackV1 callback, void* user);
    void (MMS_CALL *clearPhaseCallback)(void* context);
} MmsRenderApiV1_2;
