#pragma once
#include "plugin.h"

// Read-only service "mms.render.mme.on12" 1.0. This describes the shared
// host/MME D3D9 device, not a separate native D3D12 viewport. No GPU ownership
// or backend switch is exposed to ordinary plugin create/start callbacks.
#define MMS_MME_BACKEND_UNAVAILABLE 0u
#define MMS_MME_BACKEND_DX9 9u
#define MMS_MME_BACKEND_D3D9ON12 12u

#define MMS_MME_ON12_PENDING 0u
#define MMS_MME_ON12_ACTIVE 1u
#define MMS_MME_ON12_EXPLICIT_DX9 2u
#define MMS_MME_ON12_NO_REQUEST 3u
#define MMS_MME_ON12_MME_UNAVAILABLE 4u
#define MMS_MME_ON12_BRIDGE_UNAVAILABLE 5u
#define MMS_MME_ON12_FACTORY_FAILED 6u
#define MMS_MME_ON12_DEVICE_FAILED 7u
#define MMS_MME_ON12_CONFIRMATION_FAILED 8u
#define MMS_MME_ON12_DISABLE_FAILED 9u

typedef struct MmsMmeOn12StatusV1 {
    MmsHeader h;
    uint32_t requestedBackend;
    uint32_t actualBackend;
    uint32_t reason;
    uint32_t startupRequestEnabled;
    uint32_t mmeLoaded;
    uint32_t bridgeSupported;
} MmsMmeOn12StatusV1;

typedef struct MmsMmeOn12ApiV1 {
    MmsHeader h;
    void* context;
    // UI thread only. Initialize out->h before calling. Status is copied;
    // it remains available even when MME is missing or the request fell back.
    MmsResult (MMS_CALL *query)(void* context, MmsMmeOn12StatusV1* out);
} MmsMmeOn12ApiV1;
