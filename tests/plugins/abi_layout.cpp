#include <mms/surface.h>
#include <type_traits>
#include <mms/refresh.h>
#include <mms/refresh_binding.h>
#include <mms/refresh_state.h>
#include <mms/refresh_dependency.h>
static_assert(offsetof(MmsUiRefreshDependencyDescV1, upstream) == 8, "dependency source offset");
static_assert(offsetof(MmsUiRefreshDependencyDescV1, dependent) == 40, "dependency target offset");
static_assert(offsetof(MmsUiRefreshDependencyApiV1, unregisterDependency) == 32, "dependency unregister offset");
static_assert(std::is_standard_layout<MmsUiRefreshDependencyApiV1>::value, "dependency standard layout");
static_assert(offsetof(MmsUiStateContentDescV1, prepare) == 48, "STATE callback offset");
static_assert(offsetof(MmsUiRefreshStateContentApiV1, writePrepared) == 48, "STATE write offset");
static_assert(std::is_standard_layout<MmsUiRefreshStateContentApiV1>::value, "STATE standard layout");
static_assert(offsetof(MmsUiDataBindingDescV1, prepare) == 56, "DATA callback offset");
static_assert(offsetof(MmsUiDataDisplayV1, bytes) == 56, "DATA bytes offset");
static_assert(std::is_standard_layout<MmsUiRefreshBindingApiV1>::value, "DATA standard layout");
#include <mms/edit.h>
#include <mms/layout.h>
#include <mms/scene.h>
#include <mms/storage.h>
static_assert(sizeof(MmsSceneQueryV1) == 80 && sizeof(MmsSnapshotV1) == 40 && sizeof(MmsSceneApiV1) == 48, "scene layout");
static_assert(offsetof(MmsObjectV1, world) == 72 && sizeof(MmsObjectV1) == 168, "scene object layout");
static_assert(offsetof(MmsBoneV1, local) == 72 && sizeof(MmsBoneV1) == 336, "scene bone layout");
static_assert(sizeof(MmsVertexV1) == 128 && offsetof(MmsVertexV1, sourceVertex) == 104, "vertex layout");
static_assert(sizeof(MmsTriangleV1) == 32 && sizeof(MmsMeshV1) == 80, "mesh layout");
static_assert(sizeof(MmsWeightV1) == 120 && sizeof(MmsBindingV1) == 104, "binding layout");
static_assert(sizeof(MmsMaterialV1) == 272 && sizeof(MmsTextureV1) == 64, "material layout");
static_assert(sizeof(MmsMorphV1) == 96 && sizeof(MmsRigidV1) == 296 && sizeof(MmsJointV1) == 336 && sizeof(MmsShapeV1) == 136, "definition layout");
static_assert(sizeof(MmsMorphGroupElementV1) == 24 && sizeof(MmsMorphVertexElementV1) == 32 && sizeof(MmsMorphBoneElementV1) == 48 && sizeof(MmsMorphUvElementV1) == 32 && sizeof(MmsMorphMaterialElementV1) == 136, "morph element layout");
#include <type_traits>
static_assert(std::is_standard_layout<MmsPluginApiV1>::value, "standard layout");
static_assert(std::is_trivially_copyable<MmsUiApiV1>::value, "copyable ABI");
static_assert(offsetof(MmsHostV1, queryService) == 16, "service offset");
static_assert(offsetof(MmsCommandDescV1, context) == 104, "private callbacks offset");
static_assert(offsetof(MmsUiElementV1, surfaceId) == 104, "surface offset");
int mms_abi_cpp_probe() { return 0; }
