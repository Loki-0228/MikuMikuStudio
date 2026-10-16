#include <mms/surface.h>
#include <mms/edit.h>
#include <mms/layout.h>
#include <mms/scene.h>
_Static_assert(sizeof(MmsSceneQueryV1) == 80, "scene query");
_Static_assert(sizeof(MmsSnapshotV1) == 40, "scene snapshot");
_Static_assert(sizeof(MmsSceneApiV1) == 48, "scene service");
_Static_assert(sizeof(MmsObjectV1) == 168, "scene object");
_Static_assert(sizeof(MmsBoneV1) == 336, "scene bone");
_Static_assert(sizeof(MmsHeader) == 8, "header");
_Static_assert(sizeof(MmsId) == 16, "id");
_Static_assert(sizeof(MmsSpan) == 24, "span");
_Static_assert(sizeof(MmsValue) == 40, "value");
_Static_assert(sizeof(MmsCreateInfoV1) == 64, "create info");
_Static_assert(sizeof(MmsEventV1) == 64, "event");
_Static_assert(sizeof(MmsHostV1) == 64, "host");
_Static_assert(sizeof(MmsPluginApiV1) == 48, "entry");
_Static_assert(sizeof(MmsUiSnapshotV1) == 96, "UI snapshot");
_Static_assert(sizeof(MmsUiApiV1) == 160, "UI table");
_Static_assert(sizeof(MmsCommandInfoV1) == 104, "command info");
_Static_assert(sizeof(MmsCommandDescV1) == 128, "command registration");
_Static_assert(sizeof(MmsUiElementV1) == 136, "UI element");
_Static_assert(sizeof(MmsLayoutOpV1) == 88, "layout operation");
_Static_assert(sizeof(MmsUiAnchorV1) == 56, "anchor");
_Static_assert(sizeof(MmsUiMountV1) == 56, "mount");
_Static_assert(sizeof(MmsUiSurfaceDescV1) == 96, "surface factory");
_Static_assert(sizeof(MmsUiSurfaceApiV1) == 48, "surface service");
_Static_assert(sizeof(MmsTransactionParticipantV1) == 72, "participant");
_Static_assert(sizeof(MmsEditApiV1) == 72, "edit service");
_Static_assert(sizeof(MmsUiLayoutHintsV1) == 96, "layout hints");
_Static_assert(sizeof(MmsUiLayoutItemV1) == 248, "layout item");
_Static_assert(sizeof(MmsUiLayoutSnapshotV1) == 56, "layout snapshot");
_Static_assert(sizeof(MmsUiLayoutApiV1) == 48, "layout service");
_Static_assert(offsetof(MmsHostV1, queryService) == 16, "service offset");
_Static_assert(offsetof(MmsCommandDescV1, context) == 104, "private callbacks offset");
_Static_assert(offsetof(MmsUiElementV1, surfaceId) == 104, "surface offset");
_Static_assert(offsetof(MmsPluginApiV1, create) == 8, "entry offset");
int mms_abi_c_probe(void) { return 0; }
_Static_assert(sizeof(MmsVertexV1) == 128 && offsetof(MmsVertexV1, sourceVertex) == 104, "vertex layout");
_Static_assert(sizeof(MmsTriangleV1) == 32 && sizeof(MmsMeshV1) == 80, "mesh layout");
_Static_assert(sizeof(MmsWeightV1) == 120 && sizeof(MmsBindingV1) == 104, "binding layout");
_Static_assert(sizeof(MmsMaterialV1) == 272 && sizeof(MmsTextureV1) == 64, "material layout");
_Static_assert(sizeof(MmsMorphV1) == 96 && sizeof(MmsRigidV1) == 296 && sizeof(MmsJointV1) == 336 && sizeof(MmsShapeV1) == 136, "definition layout");
_Static_assert(sizeof(MmsMorphGroupElementV1) == 24 && sizeof(MmsMorphVertexElementV1) == 32 && sizeof(MmsMorphBoneElementV1) == 48 && sizeof(MmsMorphUvElementV1) == 32 && sizeof(MmsMorphMaterialElementV1) == 136, "morph element layout");
