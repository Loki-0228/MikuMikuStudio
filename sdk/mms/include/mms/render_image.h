#ifndef MMS_RENDER_IMAGE_H
#define MMS_RENDER_IMAGE_H
#include "plugin.h"

/* mms.render.image 1.0: immutable, host-owned CPU images. All table calls are
 * thread-safe. Query the table on the UI thread before starting worker tasks.
 * No implicit UI, scene evaluation, playback, history or GPU operation occurs. */
#define MMS_IMAGE_RGBA8 1u
#define MMS_IMAGE_BGRA8 2u
#define MMS_IMAGE_COLOR_SRGB 1u
#define MMS_IMAGE_ALPHA_OPAQUE 1u
#define MMS_IMAGE_ALPHA_STRAIGHT 2u
#define MMS_IMAGE_ALPHA_PREMULTIPLIED 3u
#define MMS_IMAGE_CAP_CPU_COPY 1u

#pragma pack(push, 8)
typedef struct MmsRenderImageCapsV1 {
    MmsHeader h;
    uint32_t flags, maxWidth, maxHeight, reserved;
    uint64_t maxImageBytes, maxClientBytes, maxTotalBytes;
    uint32_t maxImages, maxMappings;
} MmsRenderImageCapsV1;

/* Rows run top to bottom; channels follow format; bytes are copied verbatim.
 * rowStride may include padding. Input spans need only cover the last pixel.
 * No alpha conversion, color conversion or implicit premultiplication. */
typedef struct MmsRenderImageDescV1 {
    MmsHeader h;
    uint32_t width, height, format, colorSpace;
    uint32_t alphaMode, flags;
    uint64_t rowStride;
} MmsRenderImageDescV1;

typedef struct MmsRenderImageInfoV1 {
    MmsHeader h;
    uint32_t width, height, format, colorSpace;
    uint32_t alphaMode, flags;
    uint64_t rowStride, byteCount;
    uint64_t deviceGeneration; /* Zero for CPU images: device independent. */
} MmsRenderImageInfoV1;

typedef struct MmsRenderImageMapV1 {
    MmsHeader h;
    MmsHandle mapping;
    const void* pixels;
    uint64_t byteCount, rowStride;
} MmsRenderImageMapV1;

typedef struct MmsRenderImageApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsRenderImageCapsV1* out);
    MmsResult (MMS_CALL *createCpu)(void*, const MmsRenderImageDescV1*, MmsBytes pixels, MmsHandle* out);
    MmsResult (MMS_CALL *describe)(void*, MmsHandle image, MmsRenderImageInfoV1* out);
    /* A retained image has a distinct, independently releasable handle. */
    MmsResult (MMS_CALL *retain)(void*, MmsHandle image, MmsHandle* out);
    /* A mapping pins bytes independently of image handles until unmap. */
    MmsResult (MMS_CALL *mapRead)(void*, MmsHandle image, MmsRenderImageMapV1* out);
    MmsResult (MMS_CALL *unmap)(void*, MmsHandle mapping);
    MmsResult (MMS_CALL *release)(void*, MmsHandle image);
} MmsRenderImageApiV1;
#pragma pack(pop)
#endif
