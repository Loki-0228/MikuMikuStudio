#include <mms/surface.h>
#include <mms/refresh.h>
#include <mms/refresh_binding.h>
#include <mms/refresh_state.h>
#include <mms/refresh_dependency.h>
_Static_assert(sizeof(MmsUiRefreshDependencyCapabilitiesV1) == 24, "dependency capabilities");
_Static_assert(sizeof(MmsUiRefreshDependencyDescV1) == 80, "dependency descriptor");
_Static_assert(sizeof(MmsUiRefreshDependencyApiV1) == 40, "dependency API");
_Static_assert(offsetof(MmsUiRefreshDependencyDescV1, dependent) == 40, "dependency target offset");
_Static_assert(sizeof(MmsUiStateContentV1) == 64, "STATE content");
_Static_assert(sizeof(MmsUiInputStateContentV1) == 80, "INPUT STATE content");
_Static_assert(offsetof(MmsUiInputStateContentV1, content) == 0, "INPUT STATE prefix");
_Static_assert(offsetof(MmsUiInputStateContentV1, enabled) == 64, "INPUT STATE tail");
_Static_assert(sizeof(MmsUiStateContentDescV1) == 64, "STATE provider descriptor");
_Static_assert(sizeof(MmsUiStateContentCapabilitiesV1) == 40, "STATE content capabilities");
_Static_assert(sizeof(MmsUiRefreshStateContentApiV1) == 56, "STATE content API");
_Static_assert(sizeof(MmsUiRefreshBindingApiV1) == 96, "DATA binding API");
_Static_assert(sizeof(MmsUiDataCapabilitiesV1) == 56, "DATA capabilities");
_Static_assert(sizeof(MmsUiDataSourceDescV1) == 40, "DATA source descriptor");
_Static_assert(sizeof(MmsUiDataSourcePublishV1) == 48, "DATA publication");
_Static_assert(sizeof(MmsUiDataSourceStateV1) == 40, "DATA source state");
_Static_assert(sizeof(MmsUiDataPrepareV1) == 88, "DATA prepare input");
_Static_assert(sizeof(MmsUiDataBindingDescV1) == 80, "DATA binding descriptor");
_Static_assert(sizeof(MmsUiDataDisplayV1) == 72, "DATA display lease");
_Static_assert(sizeof(MmsUiDirtyRectV1) == 16, "refresh dirty rectangle");
_Static_assert(sizeof(MmsUiTargetRefV1) == 32, "UI target reference");
_Static_assert(sizeof(MmsUiRefreshCapabilitiesV1) == 40, "refresh capabilities");
_Static_assert(sizeof(MmsUiRefreshTargetV1) == 72, "refresh target");
_Static_assert(sizeof(MmsUiRefreshRequestV1) == 112, "refresh request");
_Static_assert(sizeof(MmsUiRefreshStateV1) == 40, "refresh state");
_Static_assert(sizeof(MmsUiRefreshApiV1) == 64, "refresh API");
#include <mms/edit.h>
#include <mms/layout.h>
#include <mms/scene.h>
#include <mms/storage.h>
_Static_assert(sizeof(MmsSerializerV1) == 56, "serializer");
_Static_assert(sizeof(MmsStorageApiV1) == 32, "storage");
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
