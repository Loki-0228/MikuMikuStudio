#include <mms/surface.h>
#include <type_traits>
#include <mms/refresh.h>
#include <mms/refresh_binding.h>
#include <mms/refresh_state.h>
#include <mms/refresh_dependency.h>
#include <mms/refresh_subscription.h>
#include <mms/property_refresh.h>
static_assert(offsetof(MmsUiRefreshSubscriptionDescV1, binding) == 8, "subscription binding offset");
static_assert(offsetof(MmsUiRefreshSubscriptionStateV1, lastResult) == 56, "subscription result offset");
static_assert(offsetof(MmsUiRefreshSubscriptionApiV1, unsubscribe) == 40, "subscription unsubscribe offset");
static_assert(std::is_standard_layout<MmsUiRefreshSubscriptionApiV1>::value, "subscription standard layout");
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

/* New original-property refresh ABI: x64 pack(8) binary layout. */
static_assert(sizeof(MmsUiPropertyRefV1) == 32, "property reference");
static_assert(offsetof(MmsUiPropertyRefV1, h) == 0, "property reference header offset");
static_assert(offsetof(MmsUiPropertyRefV1, property) == 8, "property reference property offset");
static_assert(offsetof(MmsUiPropertyRefV1, registrationGeneration) == 16, "property reference registration offset");
static_assert(offsetof(MmsUiPropertyRefV1, projectEpoch) == 24, "property reference epoch offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefV1>::value, "property reference standard layout");
static_assert(std::is_trivially_copyable<MmsUiPropertyRefV1>::value, "property reference copyable ABI");
static_assert(sizeof(MmsUiPropertyRefreshCapabilitiesV1) == 32, "property refresh capabilities");
static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, h) == 0, "property refresh capabilities header offset");
static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, maxSubscriptionsPerPlugin) == 8, "property refresh capabilities count offset");
static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, supportedKinds) == 12, "property refresh capabilities kinds offset");
static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, supportedFlags) == 16, "property refresh capabilities flags offset");
static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, flags) == 20, "property refresh capabilities feature offset");
static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, reserved) == 24, "property refresh capabilities reserved offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefreshCapabilitiesV1>::value, "property refresh capabilities standard layout");
static_assert(std::is_trivially_copyable<MmsUiPropertyRefreshCapabilitiesV1>::value, "property refresh capabilities copyable ABI");
static_assert(sizeof(MmsUiPropertyRefreshSubscriptionDescV1) == 88, "property refresh descriptor");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, h) == 0, "property refresh descriptor header offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, target) == 8, "property refresh descriptor target offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, property) == 40, "property refresh descriptor property offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, kinds) == 72, "property refresh descriptor kinds offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, flags) == 76, "property refresh descriptor flags offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, reserved) == 80, "property refresh descriptor reserved offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefreshSubscriptionDescV1>::value, "property refresh descriptor standard layout");
static_assert(std::is_trivially_copyable<MmsUiPropertyRefreshSubscriptionDescV1>::value, "property refresh descriptor copyable ABI");
static_assert(sizeof(MmsUiPropertyRefreshSubscriptionStateV1) == 120, "property refresh state");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, h) == 0, "property refresh state header offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, subscription) == 8, "property refresh state subscription offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, target) == 16, "property refresh state target offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, property) == 48, "property refresh state property offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, latestSequence) == 80, "property refresh state latest offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, submittedSequence) == 88, "property refresh state submitted offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, completedSequence) == 96, "property refresh state completed offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, lastResult) == 104, "property refresh state result offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, pending) == 108, "property refresh state pending offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, waitingFor) == 112, "property refresh state waiting offset");
static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, reserved) == 116, "property refresh state reserved offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefreshSubscriptionStateV1>::value, "property refresh state standard layout");
static_assert(std::is_trivially_copyable<MmsUiPropertyRefreshSubscriptionStateV1>::value, "property refresh state copyable ABI");
static_assert(sizeof(MmsUiPropertyRefreshApiV1) == 64, "property refresh API");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, h) == 0, "property refresh API header offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, context) == 8, "property refresh API context offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, getCapabilities) == 16, "property refresh API getCapabilities offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, getPropertyRef) == 24, "property refresh API getPropertyRef offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, subscribe) == 32, "property refresh API subscribe offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, notify) == 40, "property refresh API notify offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, query) == 48, "property refresh API query offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1, unsubscribe) == 56, "property refresh API unsubscribe offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefreshApiV1>::value, "property refresh API standard layout");
static_assert(std::is_trivially_copyable<MmsUiPropertyRefreshApiV1>::value, "property refresh API copyable ABI");
