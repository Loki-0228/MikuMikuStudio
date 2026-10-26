#ifndef MMS_EVALUATION_H
#define MMS_EVALUATION_H
#include "plugin.h"

/* mms.scene.evaluate 1.0. Frozen registered local bone channels only, before
 * hierarchy, IK, morphs, layers and physics. No MMS_STAGE_ANIMATED frame is
 * advertised. Future versions may append evaluated-frame/instance operations. */
#define MMS_EVALUATION_CAP_BONE_CHANNELS 1u
#define MMS_EVALUATION_BONE_LOCAL 1u
#define MMS_EVALUATION_BONE_EXTERNAL_PARENT 2u
#define MMS_EVALUATION_BONE_DERIVED 4u
#define MMS_EVALUATION_PHYSICS_DISABLED 1u
#pragma pack(push, 8)
typedef struct MmsEvaluationCapsV1 {
    MmsHeader h;
    uint32_t flags, maxSources; /* maxSources counts handles per client. */
    uint64_t maxSourceBytes, maxClientBytes, maxTotalBytes;
} MmsEvaluationCapsV1;
typedef struct MmsEvaluationSourceQueryV1 {
    MmsHeader h;
    MmsObjectRef object;
    uint64_t maxRetainedBytes; /* Zero selects the host's default. */
    uint32_t flags, reserved; /* Both zero. */
} MmsEvaluationSourceQueryV1;
typedef struct MmsEvaluationSourceInfoV1 {
    MmsHeader h;
    MmsObjectRef object;
    /* captureId is unique per successful capture, shared by retained handles.
     * hostRevision may be zero; it is metadata, never a motion content hash. */
    uint64_t captureId, hostRevision, retainedBytes;
    uint32_t boneCount, flags;
} MmsEvaluationSourceInfoV1;
/* IDs equal mms.scene bone IDs. Index is local to this immutable source. */
typedef struct MmsEvaluationBoneV1 {
    MmsHeader h;
    MmsId id;
    uint32_t flags, reserved;
} MmsEvaluationBoneV1;
typedef struct MmsBoneChannelsV1 {
    MmsHeader h;
    float position[3], rotation[4], scale[9];
    uint32_t flags, reserved;
} MmsBoneChannelsV1;
typedef struct MmsEvaluationApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    /* UI thread only, outside pending edits. The source survives model edits,
     * deletion and project switches. Capture never moves the playhead. */
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t index, MmsEvaluationBoneV1*);
    /* Time is seconds at the host's 30 fps base. Equivalent rational values
     * are normalized before conversion. Concurrent reads copy into caller storage.
     * Negative time and unimplemented bone semantics are explicitly refused. */
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId bone, MmsTime time, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
} MmsEvaluationApiV1;
#pragma pack(pop)
#endif
