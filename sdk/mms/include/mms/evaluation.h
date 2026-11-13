#ifndef MMS_EVALUATION_H
#define MMS_EVALUATION_H
#include "plugin.h"
#include "scene.h"

/* mms.scene.evaluate 1.0. Frozen registered local bone channels only, before
 * hierarchy, IK, morphs, layers and physics. No MMS_STAGE_ANIMATED frame is
 * advertised. Future versions may append evaluated-frame/instance operations. */
#define MMS_EVALUATION_CAP_BONE_CHANNELS 1u
#define MMS_EVALUATION_CAP_MORPH_WEIGHTS 2u
#define MMS_EVALUATION_CAP_DISPLAY 4u
#define MMS_EVALUATION_SELECTOR_GLOBAL 1u
#define MMS_EVALUATION_PARENT_ACTIVE 1u
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

/* 1.1: registered morph weights before group expansion or deformation.
 * The V1 table and createSource keep their bone-only semantics. */
typedef struct MmsEvaluationSourceInfoV1_1 {
    MmsHeader h;
    MmsObjectRef object;
    uint64_t captureId, hostRevision, retainedBytes;
    uint32_t boneCount, flags;
    uint32_t morphCount, reserved;
} MmsEvaluationSourceInfoV1_1;
typedef struct MmsEvaluationMorphV1 {
    MmsHeader h;
    MmsId id; /* Same as mms.scene MMS_CHANNEL_MORPHS identity. */
    uint32_t kind, flags; /* MMS_MORPH_* kind, BASE and PANEL flags. */
} MmsEvaluationMorphV1;
typedef struct MmsMorphWeightV1 {
    MmsHeader h;
    float value; /* Registered direct weight, not clamped to [0,1]. */
    uint32_t reserved;
} MmsMorphWeightV1;
typedef struct MmsEvaluationApiV1_1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    /* UI thread. channels is a nonzero subset of advertised CAP bits.
     * Both channels are captured together or no source is published. */
    MmsResult (MMS_CALL *createSourceChannels)(void*, const MmsEvaluationSourceQueryV1*, uint32_t channels, MmsHandle*);
    MmsResult (MMS_CALL *describeSourceChannels)(void*, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult (MMS_CALL *morphAt)(void*, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult (MMS_CALL *sampleMorph)(void*, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
} MmsEvaluationApiV1_1;

/* 1.2: registered visibility, IK switches and external-parent selection.
 * Parent identities are frozen metadata, not evaluated target transforms. */
typedef struct MmsEvaluationSourceInfoV1_2 {
    MmsHeader h;
    MmsObjectRef object;
    uint64_t captureId, hostRevision, retainedBytes;
    uint32_t boneCount, flags;
    uint32_t morphCount, reserved;
    uint32_t ikCount, selectorCount;
} MmsEvaluationSourceInfoV1_2;
typedef struct MmsEvaluationIkV1 {
    MmsHeader h;
    MmsId bone; /* Chain index is local to this source; bone is a scene ID. */
} MmsEvaluationIkV1;
typedef struct MmsEvaluationSelectorV1 {
    MmsHeader h;
    MmsId bone; /* Empty only for GLOBAL, which applies to the source model. */
    uint32_t flags, reserved;
} MmsEvaluationSelectorV1;
typedef struct MmsDisplayStateV1 {
    MmsHeader h;
    uint32_t visible, reserved; /* Original byte widened to uint32, not bool. */
} MmsDisplayStateV1;
typedef struct MmsIkStateV1 {
    MmsHeader h;
    uint32_t enabled, reserved; /* Original byte widened to uint32, not bool. */
} MmsIkStateV1;
typedef struct MmsParentSelectionV1 {
    MmsHeader h;
    MmsObjectRef target;
    MmsId bone;
    uint32_t windowStart, windowEnd; /* Registered frame boundaries; end 0 is unbounded. */
    uint32_t flags, reserved; /* ACTIVE requires target and bone; otherwise both empty. */
} MmsParentSelectionV1;
typedef struct MmsEvaluationApiV1_2 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    MmsResult (MMS_CALL *createSourceChannels)(void*, const MmsEvaluationSourceQueryV1*, uint32_t, MmsHandle*);
    MmsResult (MMS_CALL *describeSourceChannels)(void*, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult (MMS_CALL *morphAt)(void*, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult (MMS_CALL *sampleMorph)(void*, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
    MmsResult (MMS_CALL *describeSourceDisplay)(void*, MmsHandle, MmsEvaluationSourceInfoV1_2*);
    MmsResult (MMS_CALL *ikAt)(void*, MmsHandle, uint32_t, MmsEvaluationIkV1*);
    MmsResult (MMS_CALL *selectorAt)(void*, MmsHandle, uint32_t, MmsEvaluationSelectorV1*);
    MmsResult (MMS_CALL *sampleVisibility)(void*, MmsHandle, MmsTime, MmsDisplayStateV1*);
    MmsResult (MMS_CALL *sampleIk)(void*, MmsHandle, uint32_t, MmsTime, MmsIkStateV1*);
    MmsResult (MMS_CALL *sampleParent)(void*, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*);
} MmsEvaluationApiV1_2;

/* 1.3: native inherited-parent, ground and model-target states stay distinct.
 * sampleParent (1.2) returns UNSUPPORTED for ground, leaving output unchanged. */
#define MMS_EVALUATION_PARENT_INHERIT 0u
#define MMS_EVALUATION_PARENT_MODEL 1u
#define MMS_EVALUATION_PARENT_GROUND 2u
typedef struct MmsParentStateV1 {
    MmsHeader h;
    MmsObjectRef target;
    MmsId bone;
    uint32_t windowStart, windowEnd;
    uint32_t kind, reserved; /* MODEL requires identities; INHERIT/GROUND empty. */
} MmsParentStateV1;
typedef struct MmsEvaluationApiV1_3 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    MmsResult (MMS_CALL *createSourceChannels)(void*, const MmsEvaluationSourceQueryV1*, uint32_t, MmsHandle*);
    MmsResult (MMS_CALL *describeSourceChannels)(void*, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult (MMS_CALL *morphAt)(void*, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult (MMS_CALL *sampleMorph)(void*, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
    MmsResult (MMS_CALL *describeSourceDisplay)(void*, MmsHandle, MmsEvaluationSourceInfoV1_2*);
    MmsResult (MMS_CALL *ikAt)(void*, MmsHandle, uint32_t, MmsEvaluationIkV1*);
    MmsResult (MMS_CALL *selectorAt)(void*, MmsHandle, uint32_t, MmsEvaluationSelectorV1*);
    MmsResult (MMS_CALL *sampleVisibility)(void*, MmsHandle, MmsTime, MmsDisplayStateV1*);
    MmsResult (MMS_CALL *sampleIk)(void*, MmsHandle, uint32_t, MmsTime, MmsIkStateV1*);
    MmsResult (MMS_CALL *sampleParent)(void*, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*);
    MmsResult (MMS_CALL *sampleParentState)(void*, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*);
} MmsEvaluationApiV1_3;
/* 1.4 skeleton scene frames: registered data, CPU, no physics simulation. */
#define MMS_EVALUATION_SCENE_SKELETON 1u
#define MMS_EVALUATION_SCENE_VISIBILITY_VALID 1u

typedef struct MmsEvaluationSceneCapsV1 {
    MmsHeader h;
    uint64_t maxSourceBytes, maxFrameBytes, maxClientBytes, maxTotalBytes;
    uint64_t maxWorkspaceBytes, maxClientWorkspaceBytes, maxTotalWorkspaceBytes;
    uint32_t flags, reserved;
} MmsEvaluationSceneCapsV1;
typedef struct MmsEvaluationSceneQueryV1 {
    MmsHeader h;
    uint64_t maxRetainedBytes;
    uint32_t flags, reserved;
} MmsEvaluationSceneQueryV1;
typedef struct MmsEvaluationSceneInfoV1 {
    MmsHeader h;
    uint64_t captureId, hostRevision, retainedBytes;
    uint32_t modelCount, flags;
} MmsEvaluationSceneInfoV1;
typedef struct MmsEvaluationSceneModelV1 {
    MmsHeader h;
    MmsObjectRef object;
    uint32_t boneCount, flags, visibility, reserved;
} MmsEvaluationSceneModelV1;
typedef struct MmsEvaluationSceneBoneV1 {
    MmsHeader h;
    MmsId bone;
    uint32_t index, reserved;
} MmsEvaluationSceneBoneV1;
typedef struct MmsEvaluationFrameQueryV1 {
    MmsHeader h;
    MmsHandle source;
    MmsTime time; /* Nonnegative rational seconds; 30 frames per second. */
    uint64_t maxRetainedBytes, maxWorkspaceBytes;
    uint32_t flags, reserved;
} MmsEvaluationFrameQueryV1;
typedef struct MmsEvaluationFrameInfoV1 {
    MmsHeader h;
    uint64_t captureId, frameId, hostRevision, retainedBytes;
    MmsTime time;
    uint32_t modelCount, flags;
} MmsEvaluationFrameInfoV1;
typedef struct MmsEvaluationScenePoseV1 {
    MmsHeader h;
    MmsId bone;
    float rigid[16], scaled[16], attachmentBefore[16], attachmentAfter[16];
    float jointPosition[3];
    uint32_t reserved;
} MmsEvaluationScenePoseV1;
typedef struct MmsEvaluationApiV1_4 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    MmsResult (MMS_CALL *createSourceChannels)(void*, const MmsEvaluationSourceQueryV1*, uint32_t, MmsHandle*);
    MmsResult (MMS_CALL *describeSourceChannels)(void*, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult (MMS_CALL *morphAt)(void*, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult (MMS_CALL *sampleMorph)(void*, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
    MmsResult (MMS_CALL *describeSourceDisplay)(void*, MmsHandle, MmsEvaluationSourceInfoV1_2*);
    MmsResult (MMS_CALL *ikAt)(void*, MmsHandle, uint32_t, MmsEvaluationIkV1*);
    MmsResult (MMS_CALL *selectorAt)(void*, MmsHandle, uint32_t, MmsEvaluationSelectorV1*);
    MmsResult (MMS_CALL *sampleVisibility)(void*, MmsHandle, MmsTime, MmsDisplayStateV1*);
    MmsResult (MMS_CALL *sampleIk)(void*, MmsHandle, uint32_t, MmsTime, MmsIkStateV1*);
    MmsResult (MMS_CALL *sampleParent)(void*, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*);
    MmsResult (MMS_CALL *sampleParentState)(void*, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*);
    MmsResult (MMS_CALL *getSceneCapabilities)(void*, MmsEvaluationSceneCapsV1*);
    MmsResult (MMS_CALL *createSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSceneSource)(void*, MmsHandle, MmsEvaluationSceneInfoV1*);
    MmsResult (MMS_CALL *sceneModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *sceneBoneAt)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationSceneBoneV1*);
    MmsResult (MMS_CALL *evaluateFrame)(void*, const MmsEvaluationFrameQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeFrame)(void*, MmsHandle, MmsEvaluationFrameInfoV1*);
    MmsResult (MMS_CALL *frameModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *readBonePose)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationScenePoseV1*);
} MmsEvaluationApiV1_4;

/* 1.5: CPU-only frozen mesh geometry. A mesh source captures registered scene
 * state at one UI safe point; it can also be passed to the 1.4 scene and frame
 * functions. 1.4 createSceneSource remains skeleton-only. Unsupported source
 * or frame kinds return MMS_E_UNSUPPORTED. No physics, material evaluation,
 * graphics device or final renderer output is implied by these flags. */
#define MMS_EVALUATION_MESH_TOPOLOGY 1u
#define MMS_EVALUATION_MESH_CPU_VERTICES 2u
typedef struct MmsEvaluationMeshCapsV1 {
    MmsHeader h;
    uint32_t flags, maxItemsPerRead; /* Nonzero read limit, in records. */
    uint32_t reserved[2];
} MmsEvaluationMeshCapsV1;
typedef struct MmsEvaluationMeshInfoV1 {
    MmsHeader h;
    MmsObjectRef object; /* Same identity and generation as sceneModelAt. */
    uint32_t vertexCount, indexCount, materialCount, additionalUvCount;
    uint32_t flags, reserved;
} MmsEvaluationMeshInfoV1;
/* A material segment selects [firstIndex, firstIndex + indexCount) in the
 * same source's index stream. Each three adjacent indices form a triangle. */
typedef struct MmsEvaluationMeshMaterialV1 {
    MmsHeader h;
    uint32_t firstIndex, indexCount, reserved[2];
} MmsEvaluationMeshMaterialV1;
typedef struct MmsEvaluationMeshVertexV1 {
    MmsHeader h;
    float position[3], normal[3], uv[2], additionalUv[16];
    uint32_t reserved[2];
} MmsEvaluationMeshVertexV1;

typedef struct MmsEvaluationApiV1_5 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    MmsResult (MMS_CALL *createSourceChannels)(void*, const MmsEvaluationSourceQueryV1*, uint32_t, MmsHandle*);
    MmsResult (MMS_CALL *describeSourceChannels)(void*, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult (MMS_CALL *morphAt)(void*, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult (MMS_CALL *sampleMorph)(void*, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
    MmsResult (MMS_CALL *describeSourceDisplay)(void*, MmsHandle, MmsEvaluationSourceInfoV1_2*);
    MmsResult (MMS_CALL *ikAt)(void*, MmsHandle, uint32_t, MmsEvaluationIkV1*);
    MmsResult (MMS_CALL *selectorAt)(void*, MmsHandle, uint32_t, MmsEvaluationSelectorV1*);
    MmsResult (MMS_CALL *sampleVisibility)(void*, MmsHandle, MmsTime, MmsDisplayStateV1*);
    MmsResult (MMS_CALL *sampleIk)(void*, MmsHandle, uint32_t, MmsTime, MmsIkStateV1*);
    MmsResult (MMS_CALL *sampleParent)(void*, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*);
    MmsResult (MMS_CALL *sampleParentState)(void*, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*);
    MmsResult (MMS_CALL *getSceneCapabilities)(void*, MmsEvaluationSceneCapsV1*);
    MmsResult (MMS_CALL *createSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSceneSource)(void*, MmsHandle, MmsEvaluationSceneInfoV1*);
    MmsResult (MMS_CALL *sceneModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *sceneBoneAt)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationSceneBoneV1*);
    MmsResult (MMS_CALL *evaluateFrame)(void*, const MmsEvaluationFrameQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeFrame)(void*, MmsHandle, MmsEvaluationFrameInfoV1*);
    MmsResult (MMS_CALL *frameModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *readBonePose)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationScenePoseV1*);

    MmsResult (MMS_CALL *getMeshCapabilities)(void*, MmsEvaluationMeshCapsV1*);
    /* UI thread only, outside pending edits. Query flags and reserved are zero.
     * The source has the same ownership and release rules as scene sources. */
    MmsResult (MMS_CALL *createMeshSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeMesh)(void*, MmsHandle source, uint32_t model, MmsEvaluationMeshInfoV1*);
    /* Every read copies exactly count records from [first, first + count).
     * count must not exceed maxItemsPerRead or the remaining extent. For zero
     * count, first may equal the extent and out may be NULL. Outputs are left
     * unchanged on any failure; records are host-initialized including h and
     * reserved, so callers need not initialize the output array. An invalid
     * handle, model or range is never silently truncated. The caller owns an
     * adequately sized, writable output array for the duration of the call.
     * Model indices match sceneModelAt/frameModelAt; indices are local to that
     * model and captureId. Additional UV groups above additionalUvCount are
     * zero. Calls borrow the handle and do not return internal pointers. */
    MmsResult (MMS_CALL *readMeshIndices)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, uint32_t* out);
    MmsResult (MMS_CALL *readMeshMaterials)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshMaterialV1* out);
    MmsResult (MMS_CALL *readMeshBindVertices)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshVertexV1* out);
    MmsResult (MMS_CALL *readMeshVertices)(void*, MmsHandle frame, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshVertexV1* out);
} MmsEvaluationApiV1_5;

/* 1.6: immutable material values evaluated at the same time as the mesh.
 * Format flags describe the loaded model format, not a physics simulation.
 * PMX material morph contributions follow the native one-level group rules;
 * PMD values are static, with texture/sphere/toon add=0 and multiply=1.
 * Values do not include sampled textures, lighting, resolved toon colors,
 * edge rendering, physics, or a final rendered color. Unsupported source and
 * frame kinds return MMS_E_UNSUPPORTED; unsupported inputs are not approximated. */
#define MMS_EVALUATION_MATERIAL_CAP_CPU_VALUES 1u
#define MMS_EVALUATION_MATERIAL_CAP_PMD 2u
#define MMS_EVALUATION_MATERIAL_CAP_PMX 4u
#define MMS_EVALUATION_MATERIAL_FORMAT_PMD 1u
#define MMS_EVALUATION_MATERIAL_FORMAT_PMX 2u
typedef struct MmsEvaluationMaterialCapsV1 {
    MmsHeader h;
    uint32_t flags, maxItemsPerRead;
    uint32_t reserved[2];
} MmsEvaluationMaterialCapsV1;
typedef struct MmsEvaluationMaterialIdV1 {
    MmsHeader h;
    MmsId material;
    uint32_t index, reserved;
} MmsEvaluationMaterialIdV1;
typedef struct MmsEvaluationMaterialV1 {
    MmsHeader h;
    MmsId material;
    uint32_t index, flags; /* exactly one MATERIAL_FORMAT_* value */
    /* The native record's original diffuse RGB remains distinct from the
     * morphed diffuseMirror RGB; the latter's fourth lane is diffuse alpha. */
    float nativeDiffuse[3], diffuseMirrorAndAlpha[4];
    float specular[3], ambient[3], specularPower, edgeColor[4], edgeSize;
    /* Preserve additive and multiplicative texture inputs separately. */
    float textureAdd[4], textureMultiply[4], sphereAdd[4], sphereMultiply[4];
    float toonAdd[4], toonMultiply[4];
    uint32_t reserved[2];
} MmsEvaluationMaterialV1;

typedef struct MmsEvaluationApiV1_6 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    MmsResult (MMS_CALL *createSourceChannels)(void*, const MmsEvaluationSourceQueryV1*, uint32_t, MmsHandle*);
    MmsResult (MMS_CALL *describeSourceChannels)(void*, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult (MMS_CALL *morphAt)(void*, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult (MMS_CALL *sampleMorph)(void*, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
    MmsResult (MMS_CALL *describeSourceDisplay)(void*, MmsHandle, MmsEvaluationSourceInfoV1_2*);
    MmsResult (MMS_CALL *ikAt)(void*, MmsHandle, uint32_t, MmsEvaluationIkV1*);
    MmsResult (MMS_CALL *selectorAt)(void*, MmsHandle, uint32_t, MmsEvaluationSelectorV1*);
    MmsResult (MMS_CALL *sampleVisibility)(void*, MmsHandle, MmsTime, MmsDisplayStateV1*);
    MmsResult (MMS_CALL *sampleIk)(void*, MmsHandle, uint32_t, MmsTime, MmsIkStateV1*);
    MmsResult (MMS_CALL *sampleParent)(void*, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*);
    MmsResult (MMS_CALL *sampleParentState)(void*, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*);
    MmsResult (MMS_CALL *getSceneCapabilities)(void*, MmsEvaluationSceneCapsV1*);
    MmsResult (MMS_CALL *createSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSceneSource)(void*, MmsHandle, MmsEvaluationSceneInfoV1*);
    MmsResult (MMS_CALL *sceneModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *sceneBoneAt)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationSceneBoneV1*);
    MmsResult (MMS_CALL *evaluateFrame)(void*, const MmsEvaluationFrameQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeFrame)(void*, MmsHandle, MmsEvaluationFrameInfoV1*);
    MmsResult (MMS_CALL *frameModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *readBonePose)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationScenePoseV1*);

    MmsResult (MMS_CALL *getMeshCapabilities)(void*, MmsEvaluationMeshCapsV1*);
    /* UI thread only, outside pending edits. Query flags and reserved are zero.
     * The source has the same ownership and release rules as scene sources. */
    MmsResult (MMS_CALL *createMeshSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeMesh)(void*, MmsHandle source, uint32_t model, MmsEvaluationMeshInfoV1*);
    /* Every read copies exactly count records from [first, first + count).
     * count must not exceed maxItemsPerRead or the remaining extent. For zero
     * count, first may equal the extent and out may be NULL. Outputs are left
     * unchanged on any failure; records are host-initialized including h and
     * reserved, so callers need not initialize the output array. An invalid
     * handle, model or range is never silently truncated. The caller owns an
     * adequately sized, writable output array for the duration of the call.
     * Model indices match sceneModelAt/frameModelAt; indices are local to that
     * model and captureId. Additional UV groups above additionalUvCount are
     * zero. Calls borrow the handle and do not return internal pointers. */
    MmsResult (MMS_CALL *readMeshIndices)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, uint32_t* out);
    MmsResult (MMS_CALL *readMeshMaterials)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshMaterialV1* out);
    MmsResult (MMS_CALL *readMeshBindVertices)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshVertexV1* out);
    MmsResult (MMS_CALL *readMeshVertices)(void*, MmsHandle frame, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshVertexV1* out);
    /* A separate opt-in source includes the 1.5 mesh data and frozen material
     * values from the same registered scene. UI thread only; flags/reserved in
     * the scene query must be zero. Existing 1.5 creation remains unchanged. */
    MmsResult (MMS_CALL *getMaterialCapabilities)(void*, MmsEvaluationMaterialCapsV1*);
    MmsResult (MMS_CALL *createMaterialSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    /* Stable ID of one source material, matching BIND's MATERIALS identity.
     * Model and material indices are scoped to this source capture. */
    MmsResult (MMS_CALL *materialAt)(void*, MmsHandle source, uint32_t model, uint32_t index, MmsEvaluationMaterialIdV1*);
    /* Copies count frame records atomically. count must not exceed the
     * capability limit or the remaining model material extent. With count=0,
     * first may equal the extent and out may be NULL. Handle, model and range
     * are validated even for an empty read. On failure no output is changed.
     * Each returned material ID is the identity frozen with this frame. Frame
     * data remains valid after the source handle and live model are released.
     * Caller owns an adequately sized writable array. No host pointer escapes. */
    MmsResult (MMS_CALL *readFrameMaterials)(void*, MmsHandle frame, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMaterialV1* out);
} MmsEvaluationApiV1_6;
/* 1.7 source-only runtime references. Paths are UTF-16 code units, terminated
 * within each fixed array. Toon table bytes are verbatim, possibly non-NUL
 * terminated and not necessarily valid CP932. These are not frozen pixels,
 * resource identities, or guaranteed original file fields. */
#define MMS_EVALUATION_REFERENCE_CAP_RUNTIME_FIELDS 1u
typedef struct MmsEvaluationReferenceCapsV1 { MmsHeader h; uint32_t flags; uint32_t reserved; } MmsEvaluationReferenceCapsV1;
typedef struct MmsEvaluationMaterialReferencesV1 {
    MmsHeader h; MmsId material; uint32_t index; uint32_t flags;
    uint16_t texturePath[280], spherePath[280], toonPath[512];
    uint8_t toonReference, sphereMode, materialFlags, reserved;
} MmsEvaluationMaterialReferencesV1;
typedef struct MmsEvaluationToonTableV1 {
    MmsHeader h; uint16_t modelDirectory[256]; uint8_t fileNames[10][100];
} MmsEvaluationToonTableV1;

typedef struct MmsEvaluationApiV1_7 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *getCapabilities)(void*, MmsEvaluationCapsV1*);
    MmsResult (MMS_CALL *createSource)(void*, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSource)(void*, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult (MMS_CALL *boneAt)(void*, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult (MMS_CALL *sampleBone)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult (MMS_CALL *retain)(void*, MmsHandle, MmsHandle*);
    MmsResult (MMS_CALL *release)(void*, MmsHandle);
    MmsResult (MMS_CALL *createSourceChannels)(void*, const MmsEvaluationSourceQueryV1*, uint32_t, MmsHandle*);
    MmsResult (MMS_CALL *describeSourceChannels)(void*, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult (MMS_CALL *morphAt)(void*, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult (MMS_CALL *sampleMorph)(void*, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
    MmsResult (MMS_CALL *describeSourceDisplay)(void*, MmsHandle, MmsEvaluationSourceInfoV1_2*);
    MmsResult (MMS_CALL *ikAt)(void*, MmsHandle, uint32_t, MmsEvaluationIkV1*);
    MmsResult (MMS_CALL *selectorAt)(void*, MmsHandle, uint32_t, MmsEvaluationSelectorV1*);
    MmsResult (MMS_CALL *sampleVisibility)(void*, MmsHandle, MmsTime, MmsDisplayStateV1*);
    MmsResult (MMS_CALL *sampleIk)(void*, MmsHandle, uint32_t, MmsTime, MmsIkStateV1*);
    MmsResult (MMS_CALL *sampleParent)(void*, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*);
    MmsResult (MMS_CALL *sampleParentState)(void*, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*);
    MmsResult (MMS_CALL *getSceneCapabilities)(void*, MmsEvaluationSceneCapsV1*);
    MmsResult (MMS_CALL *createSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeSceneSource)(void*, MmsHandle, MmsEvaluationSceneInfoV1*);
    MmsResult (MMS_CALL *sceneModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *sceneBoneAt)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationSceneBoneV1*);
    MmsResult (MMS_CALL *evaluateFrame)(void*, const MmsEvaluationFrameQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeFrame)(void*, MmsHandle, MmsEvaluationFrameInfoV1*);
    MmsResult (MMS_CALL *frameModelAt)(void*, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult (MMS_CALL *readBonePose)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationScenePoseV1*);

    MmsResult (MMS_CALL *getMeshCapabilities)(void*, MmsEvaluationMeshCapsV1*);
    /* UI thread only, outside pending edits. Query flags and reserved are zero.
     * The source has the same ownership and release rules as scene sources. */
    MmsResult (MMS_CALL *createMeshSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *describeMesh)(void*, MmsHandle source, uint32_t model, MmsEvaluationMeshInfoV1*);
    /* Every read copies exactly count records from [first, first + count).
     * count must not exceed maxItemsPerRead or the remaining extent. For zero
     * count, first may equal the extent and out may be NULL. Outputs are left
     * unchanged on any failure; records are host-initialized including h and
     * reserved, so callers need not initialize the output array. An invalid
     * handle, model or range is never silently truncated. The caller owns an
     * adequately sized, writable output array for the duration of the call.
     * Model indices match sceneModelAt/frameModelAt; indices are local to that
     * model and captureId. Additional UV groups above additionalUvCount are
     * zero. Calls borrow the handle and do not return internal pointers. */
    MmsResult (MMS_CALL *readMeshIndices)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, uint32_t* out);
    MmsResult (MMS_CALL *readMeshMaterials)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshMaterialV1* out);
    MmsResult (MMS_CALL *readMeshBindVertices)(void*, MmsHandle source, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshVertexV1* out);
    MmsResult (MMS_CALL *readMeshVertices)(void*, MmsHandle frame, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMeshVertexV1* out);
    /* A separate opt-in source includes the 1.5 mesh data and frozen material
     * values from the same registered scene. UI thread only; flags/reserved in
     * the scene query must be zero. Existing 1.5 creation remains unchanged. */
    MmsResult (MMS_CALL *getMaterialCapabilities)(void*, MmsEvaluationMaterialCapsV1*);
    MmsResult (MMS_CALL *createMaterialSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    /* Stable ID of one source material, matching BIND's MATERIALS identity.
     * Model and material indices are scoped to this source capture. */
    MmsResult (MMS_CALL *materialAt)(void*, MmsHandle source, uint32_t model, uint32_t index, MmsEvaluationMaterialIdV1*);
    /* Copies count frame records atomically. count must not exceed the
     * capability limit or the remaining model material extent. With count=0,
     * first may equal the extent and out may be NULL. Handle, model and range
     * are validated even for an empty read. On failure no output is changed.
     * Each returned material ID is the identity frozen with this frame. Frame
     * data remains valid after the source handle and live model are released.
     * Caller owns an adequately sized writable array. No host pointer escapes. */
    MmsResult (MMS_CALL *readFrameMaterials)(void*, MmsHandle frame, uint32_t model, uint32_t first, uint32_t count, MmsEvaluationMaterialV1* out);
    MmsResult (MMS_CALL *getReferenceCapabilities)(void*, MmsEvaluationReferenceCapsV1*);
    MmsResult (MMS_CALL *createReferenceSceneSource)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*);
    MmsResult (MMS_CALL *readMaterialReferences)(void*, MmsHandle, uint32_t model, uint32_t index, MmsEvaluationMaterialReferencesV1*);
    MmsResult (MMS_CALL *readToonTable)(void*, MmsHandle, uint32_t model, MmsEvaluationToonTableV1*);
} MmsEvaluationApiV1_7;

#pragma pack(pop)
#endif
