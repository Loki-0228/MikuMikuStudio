#ifndef MMS_SCENE_H
#define MMS_SCENE_H
#include "plugin.h"
#define MMS_STAGE_BIND 0u
#define MMS_STAGE_MORPH 1u
#define MMS_STAGE_ANIMATED 2u
#define MMS_STAGE_PHYSICS 3u
#define MMS_STAGE_LAYER 4u
#define MMS_CHANNEL_MESH 1u
#define MMS_CHANNEL_BONES 2u
#define MMS_CHANNEL_BINDING 4u
#define MMS_CHANNEL_RIGIDS 8u
#define MMS_CHANNEL_JOINTS 16u
#define MMS_CHANNEL_SHAPES 32u
#define MMS_CHANNEL_MORPHS 64u
#define MMS_CHANNEL_MATERIALS 128u
#define MMS_CHANNEL_TEXTURES 256u
#define MMS_OBJECT_MODEL 1u
#define MMS_OBJECT_ACCESSORY 2u
#pragma pack(push, 8)
typedef struct MmsSceneQueryV1 {
	MmsHeader h;
	uint32_t stage, flags;
	uint64_t channels;
	MmsId layerId;
	MmsTime time;
	MmsSpan objects;
} MmsSceneQueryV1;
typedef struct MmsSnapshotV1 {
	MmsHeader h;
	MmsHandle handle;
	uint64_t sceneRevision, structureRevision, poseRevision;
} MmsSnapshotV1;
typedef struct MmsObjectV1 {
	MmsHeader h;
	MmsObjectRef object;
	uint32_t kind, visible;
	MmsString name, sourcePath;
	float world[16];
	uint64_t structureRevision, meshRevision, materialRevision, poseRevision;
} MmsObjectV1;
typedef struct MmsVertexV1 {
	float position[3], normal[3], uv[2], additionalUv[16], edgeScale;
	MmsId sourceVertex;
	uint32_t flags, reserved;
} MmsVertexV1;
typedef struct MmsTriangleV1 { uint32_t vertex[3], reserved; MmsId material; } MmsTriangleV1;
typedef struct MmsMeshV1 { MmsHeader h; MmsSpan vertices, triangles; float boundsMin[3], boundsMax[3]; } MmsMeshV1;
typedef struct MmsWeightV1 {
	MmsId bones[4];
	float weights[4], sdefCenter[3], sdefR0[3], sdefR1[3];
	uint32_t type;
} MmsWeightV1;
typedef struct MmsBoneV1 {
	MmsHeader h;
	MmsId id, sourceId, parentId;
	MmsString name;
	float local[16], world[16], bindWorld[16], inverseBindWorld[16];
	uint32_t flags, reserved;
} MmsBoneV1;
typedef struct MmsBindingV1 { MmsHeader h; MmsSpan bindVertices, morphVertices, weights, vertexBindings; } MmsBindingV1;
typedef struct MmsMorphV1 {
	MmsHeader h;
	MmsId id, sourceId;
	MmsString name;
	uint32_t kind, flags;
	float value;
	uint32_t reserved;
	MmsSpan elements;
} MmsMorphV1;
typedef struct MmsRigidV1 {
	MmsHeader h;
	MmsId id, sourceId, boneId, shapeId;
	float world[16], bodyToBone[16], mass, inertia[9], linearVelocity[3], angularVelocity[3], damping[2], friction, restitution;
	uint32_t mode, group, collisionMask, flags;
} MmsRigidV1;
typedef struct MmsJointV1 {
	MmsHeader h;
	MmsId id, sourceId, objectA, bodyA, objectB, bodyB;
	float frameA[16], frameB[16], linearLower[3], linearUpper[3], angularLower[3], angularUpper[3], springs[6], damping[6];
	uint32_t kind, collideConnected;
} MmsJointV1;
typedef struct MmsShapeV1 {
	MmsHeader h;
	MmsId id, sourceId;
	uint32_t kind, flags;
	float size[3];
	uint32_t reserved;
	MmsSpan vertices, indices, children;
} MmsShapeV1;
typedef struct MmsMaterialV1 {
	MmsHeader h;
	MmsId id, sourceId;
	MmsString name;
	float diffuse[4], ambient[3], specular[3], power, edgeColor[4], edgeSize;
	uint32_t flags, sphereMode;
	MmsId texture, sphere, toon;
	float textureMul[4], textureAdd[4], sphereMul[4], sphereAdd[4], toonMul[4], toonAdd[4];
} MmsMaterialV1;
typedef struct MmsTextureV1 {
	MmsHeader h;
	MmsId id;
	MmsString path;
	uint32_t width, height, format, colorSpace;
	uint64_t revision;
} MmsTextureV1;
typedef struct MmsSceneApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *acquire)(void*, const MmsSceneQueryV1*, MmsSnapshotV1*);
	void (MMS_CALL *release)(void*, MmsHandle);
	MmsResult (MMS_CALL *objects)(void*, MmsHandle, MmsSpan*);
	MmsResult (MMS_CALL *readChannel)(void*, MmsHandle, MmsObjectRef, uint32_t, MmsSpan*);
} MmsSceneApiV1;
#pragma pack(pop)
#endif
