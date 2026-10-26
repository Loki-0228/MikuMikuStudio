#include <mms/surface.h>
#include <type_traits>
#include <mms/refresh.h>
#include <mms/refresh_binding.h>
#include <mms/refresh_state.h>
#include <mms/refresh_dependency.h>
#include <mms/refresh_subscription.h>
#include <mms/property_refresh.h>
#include <mms/render_image.h>
static_assert(sizeof(MmsRenderImageCapsV1) == 56, "image capabilities");
static_assert(sizeof(MmsRenderImageDescV1) == 40, "image descriptor");
static_assert(sizeof(MmsRenderImageInfoV1) == 56, "image info");
static_assert(sizeof(MmsRenderImageMapV1) == 40, "image mapping");
static_assert(sizeof(MmsRenderImageApiV1) == 72, "image API");
static_assert(offsetof(MmsRenderImageMapV1, pixels) == 16, "image pixels offset");
static_assert(offsetof(MmsRenderImageApiV1, createCpu) == 24, "image creation offset");
static_assert(std::is_standard_layout<MmsRenderImageApiV1>::value && std::is_trivially_copyable<MmsRenderImageApiV1>::value, "image API POD");
static_assert(std::is_same<decltype(((MmsRenderImageApiV1*)nullptr)->createCpu), MmsResult (MMS_CALL *)(void*, const MmsRenderImageDescV1*, MmsBytes, MmsHandle*)>::value, "image create signature");
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

static_assert(sizeof(MmsUiRefreshScopedSubscriptionDescV1) == 80, "scoped subscription descriptor");
static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, binding) == 8 && offsetof(MmsUiRefreshScopedSubscriptionDescV1, seed) == 16, "scoped trigger and seed identity");
static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, scope) == 48 && offsetof(MmsUiRefreshScopedSubscriptionDescV1, kinds) == 52, "scoped request fields");
static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, flags) == 56 && offsetof(MmsUiRefreshScopedSubscriptionDescV1, reserved) == 60 && offsetof(MmsUiRefreshScopedSubscriptionDescV1, reservedTail) == 64, "scoped request reserved fields");
static_assert(sizeof(MmsUiRefreshScopedSubscriptionStateV1) == 160 && offsetof(MmsUiRefreshScopedSubscriptionStateV1, seed) == sizeof(MmsUiRefreshSubscriptionStateV1), "scoped state append");
#define MMS_SCOPED_STATE_PREFIX(field) static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, field) == offsetof(MmsUiRefreshSubscriptionStateV1, field), "scoped state prefix " #field)
MMS_SCOPED_STATE_PREFIX(h); MMS_SCOPED_STATE_PREFIX(subscription); MMS_SCOPED_STATE_PREFIX(binding);
MMS_SCOPED_STATE_PREFIX(projectEpoch); MMS_SCOPED_STATE_PREFIX(latestRevision); MMS_SCOPED_STATE_PREFIX(submittedRevision);
MMS_SCOPED_STATE_PREFIX(completedRevision); MMS_SCOPED_STATE_PREFIX(lastResult); MMS_SCOPED_STATE_PREFIX(pending); MMS_SCOPED_STATE_PREFIX(reserved);
#undef MMS_SCOPED_STATE_PREFIX
static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, scope) == 104 && offsetof(MmsUiRefreshScopedSubscriptionStateV1, kinds) == 108, "scoped state request fields");
static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, workGeneration) == 112 && offsetof(MmsUiRefreshScopedSubscriptionStateV1, submittedWorkGeneration) == 120, "scoped work identities");
static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, completedWorkGeneration) == 128 && offsetof(MmsUiRefreshScopedSubscriptionStateV1, submittedDependencyRevision) == 136, "scoped completion identities");
static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, waitingFor) == 144 && offsetof(MmsUiRefreshScopedSubscriptionStateV1, flags) == 148 && offsetof(MmsUiRefreshScopedSubscriptionStateV1, reservedTail) == 152, "scoped state status fields");
static_assert(sizeof(MmsUiRefreshSubscriptionApiV1_1) == 64, "scoped subscription API");
#define MMS_SCOPED_API_PREFIX(field) static_assert(offsetof(MmsUiRefreshSubscriptionApiV1_1, field) == offsetof(MmsUiRefreshSubscriptionApiV1, field), "scoped API prefix " #field)
MMS_SCOPED_API_PREFIX(h); MMS_SCOPED_API_PREFIX(context); MMS_SCOPED_API_PREFIX(getCapabilities);
MMS_SCOPED_API_PREFIX(subscribe); MMS_SCOPED_API_PREFIX(query); MMS_SCOPED_API_PREFIX(unsubscribe);
#undef MMS_SCOPED_API_PREFIX
static_assert(offsetof(MmsUiRefreshSubscriptionApiV1_1, subscribeScoped) == 48 && offsetof(MmsUiRefreshSubscriptionApiV1_1, queryScoped) == 56, "scoped API appended slots");
static_assert(std::is_standard_layout<MmsUiRefreshScopedSubscriptionDescV1>::value && std::is_trivially_copyable<MmsUiRefreshScopedSubscriptionDescV1>::value, "scoped descriptor POD");
static_assert(std::is_standard_layout<MmsUiRefreshScopedSubscriptionStateV1>::value && std::is_trivially_copyable<MmsUiRefreshScopedSubscriptionStateV1>::value, "scoped state POD");
static_assert(std::is_standard_layout<MmsUiRefreshSubscriptionApiV1_1>::value && std::is_trivially_copyable<MmsUiRefreshSubscriptionApiV1_1>::value, "scoped API POD");

/* mms.ui.refresh.subscribe 1.1 compile-time compatibility: natural alignment,
   scope and WAIT-bit values, unchanged global ABI minor, exact callback types. */
static_assert(alignof(MmsUiRefreshScopedSubscriptionDescV1) == 8, "scoped descriptor natural alignment");
static_assert(alignof(MmsUiRefreshScopedSubscriptionStateV1) == 8, "scoped state natural alignment");
static_assert(alignof(MmsUiRefreshSubscriptionApiV1_1) == 8, "scoped API natural alignment");
static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, h) == 0, "scoped descriptor header offset");
static_assert(MMS_UI_REFRESH_SELF == 1u && MMS_UI_REFRESH_SUBTREE == 2u && MMS_UI_REFRESH_DEPENDENTS == 4u, "refresh scope values");
static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH == 1u && MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET == 2u && MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY == 4u, "scoped wait bits 1-3");
static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE == 8u && MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER == 16u && MMS_UI_REFRESH_SUBSCRIPTION_WAIT_BINDING == 32u, "scoped wait bits 4-6");
static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE == 64u && MMS_UI_REFRESH_SUBSCRIPTION_WAIT_DISPLAY_CAPACITY == 128u, "scoped wait bits 7-8");
static_assert(MMS_ABI_MAJOR == 1u && MMS_ABI_MINOR == 0u, "service minor 1 keeps ABI minor 0");
static_assert(sizeof(MmsUiRefreshSubscriptionApiV1_1) == sizeof(MmsUiRefreshSubscriptionApiV1) + 2u * sizeof(void*), "service minor 1 only appends its own table");
static_assert(std::is_same<decltype(((MmsUiRefreshSubscriptionApiV1_1*)nullptr)->subscribeScoped), MmsResult (MMS_CALL *)(void*, const MmsUiRefreshScopedSubscriptionDescV1*, MmsHandle*)>::value, "scoped subscribe exact signature");
static_assert(std::is_same<decltype(((MmsUiRefreshSubscriptionApiV1_1*)nullptr)->queryScoped), MmsResult (MMS_CALL *)(void*, MmsHandle, MmsUiRefreshScopedSubscriptionStateV1*)>::value, "scoped query exact signature");
/* mms.ui.refresh.property 1.1 compile-time ABI: scoped capabilities, descriptor,
   state and API append-only layouts, POD properties, 8-byte natural alignment,
   frozen WAIT bits and exact callback types. All 1.0 assertions above remain in force. */
static_assert(sizeof(MmsUiPropertyRefreshScopedCapabilitiesV1) == 40, "scoped property refresh capabilities");
static_assert(alignof(MmsUiPropertyRefreshScopedCapabilitiesV1) == 8, "scoped property refresh capabilities alignment");
static_assert(offsetof(MmsUiPropertyRefreshScopedCapabilitiesV1, supportedScopes) == 32, "scoped property refresh capabilities scopes offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedCapabilitiesV1, reservedTail) == 36, "scoped property refresh capabilities tail offset");
#define MMS_PROPERTY_SCOPED_CAPS_PREFIX(field) static_assert(offsetof(MmsUiPropertyRefreshScopedCapabilitiesV1, field) == offsetof(MmsUiPropertyRefreshCapabilitiesV1, field), "scoped property refresh capabilities prefix " #field)
MMS_PROPERTY_SCOPED_CAPS_PREFIX(h); MMS_PROPERTY_SCOPED_CAPS_PREFIX(maxSubscriptionsPerPlugin);
MMS_PROPERTY_SCOPED_CAPS_PREFIX(supportedKinds); MMS_PROPERTY_SCOPED_CAPS_PREFIX(supportedFlags);
MMS_PROPERTY_SCOPED_CAPS_PREFIX(flags); MMS_PROPERTY_SCOPED_CAPS_PREFIX(reserved);
#undef MMS_PROPERTY_SCOPED_CAPS_PREFIX
static_assert(std::is_standard_layout<MmsUiPropertyRefreshScopedCapabilitiesV1>::value && std::is_trivially_copyable<MmsUiPropertyRefreshScopedCapabilitiesV1>::value, "scoped property refresh capabilities POD");
static_assert(sizeof(MmsUiPropertyRefreshScopedSubscriptionDescV1) == 96, "scoped property refresh descriptor");
static_assert(alignof(MmsUiPropertyRefreshScopedSubscriptionDescV1) == 8, "scoped property refresh descriptor alignment");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, h) == 0, "scoped property refresh descriptor header offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, property) == 8, "scoped property refresh descriptor property offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, seed) == 40, "scoped property refresh descriptor seed offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, scope) == 72, "scoped property refresh descriptor scope offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, kinds) == 76, "scoped property refresh descriptor kinds offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, flags) == 80, "scoped property refresh descriptor flags offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, reserved) == 84, "scoped property refresh descriptor reserved offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, reservedTail) == 88, "scoped property refresh descriptor tail offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefreshScopedSubscriptionDescV1>::value && std::is_trivially_copyable<MmsUiPropertyRefreshScopedSubscriptionDescV1>::value, "scoped property refresh descriptor POD");
static_assert(sizeof(MmsUiPropertyRefreshScopedSubscriptionStateV1) == 176, "scoped property refresh state");
static_assert(alignof(MmsUiPropertyRefreshScopedSubscriptionStateV1) == 8, "scoped property refresh state alignment");
#define MMS_PROPERTY_SCOPED_STATE_PREFIX(field) static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, field) == offsetof(MmsUiPropertyRefreshSubscriptionStateV1, field), "scoped property refresh state prefix " #field)
MMS_PROPERTY_SCOPED_STATE_PREFIX(h); MMS_PROPERTY_SCOPED_STATE_PREFIX(subscription);
MMS_PROPERTY_SCOPED_STATE_PREFIX(target); MMS_PROPERTY_SCOPED_STATE_PREFIX(property);
MMS_PROPERTY_SCOPED_STATE_PREFIX(latestSequence); MMS_PROPERTY_SCOPED_STATE_PREFIX(submittedSequence);
MMS_PROPERTY_SCOPED_STATE_PREFIX(completedSequence); MMS_PROPERTY_SCOPED_STATE_PREFIX(lastResult);
MMS_PROPERTY_SCOPED_STATE_PREFIX(pending); MMS_PROPERTY_SCOPED_STATE_PREFIX(waitingFor);
MMS_PROPERTY_SCOPED_STATE_PREFIX(reserved);
#undef MMS_PROPERTY_SCOPED_STATE_PREFIX
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, scope) == 120, "scoped property refresh state scope offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, kinds) == 124, "scoped property refresh state kinds offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, flags) == 128, "scoped property refresh state flags offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, reservedTail0) == 132, "scoped property refresh state reserved tail0 offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, workGeneration) == 136, "scoped property refresh state work generation offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, submittedWorkGeneration) == 144, "scoped property refresh state submitted work generation offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, completedWorkGeneration) == 152, "scoped property refresh state completed work generation offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, submittedDependencyRevision) == 160, "scoped property refresh state dependency revision offset");
static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, reservedTail) == 168, "scoped property refresh state tail offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefreshScopedSubscriptionStateV1>::value && std::is_trivially_copyable<MmsUiPropertyRefreshScopedSubscriptionStateV1>::value, "scoped property refresh state POD");
static_assert(sizeof(MmsUiPropertyRefreshApiV1_1) == 88, "scoped property refresh API");
static_assert(sizeof(MmsUiPropertyRefreshApiV1_1) == sizeof(MmsUiPropertyRefreshApiV1) + 3u * sizeof(void*), "scoped property refresh API appends exactly three slots");
static_assert(alignof(MmsUiPropertyRefreshApiV1_1) == 8, "scoped property refresh API alignment");
#define MMS_PROPERTY_SCOPED_API_PREFIX(field) static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, field) == offsetof(MmsUiPropertyRefreshApiV1, field), "scoped property refresh API prefix " #field)
MMS_PROPERTY_SCOPED_API_PREFIX(h); MMS_PROPERTY_SCOPED_API_PREFIX(context);
MMS_PROPERTY_SCOPED_API_PREFIX(getCapabilities); MMS_PROPERTY_SCOPED_API_PREFIX(getPropertyRef);
MMS_PROPERTY_SCOPED_API_PREFIX(subscribe); MMS_PROPERTY_SCOPED_API_PREFIX(notify);
MMS_PROPERTY_SCOPED_API_PREFIX(query); MMS_PROPERTY_SCOPED_API_PREFIX(unsubscribe);
#undef MMS_PROPERTY_SCOPED_API_PREFIX
static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, getScopedCapabilities) == 64, "scoped property refresh API getScopedCapabilities offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, subscribeScoped) == 72, "scoped property refresh API subscribeScoped offset");
static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, queryScoped) == 80, "scoped property refresh API queryScoped offset");
static_assert(std::is_standard_layout<MmsUiPropertyRefreshApiV1_1>::value && std::is_trivially_copyable<MmsUiPropertyRefreshApiV1_1>::value, "scoped property refresh API POD");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_BINDING == 1u, "property WAIT binding bit");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_TARGET == 2u, "property WAIT target bit");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY == 4u, "property WAIT capacity bit");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER == 8u, "property WAIT consumer bit");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE == 16u, "property WAIT host wake bit");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_GRAPH == 32u, "property WAIT graph bit");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE == 64u, "property WAIT source bit");
static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY == 128u, "property WAIT display capacity bit");
static_assert(std::is_same<decltype(((MmsUiPropertyRefreshApiV1_1*)nullptr)->getScopedCapabilities), MmsResult (MMS_CALL *)(void*, MmsUiPropertyRefreshScopedCapabilitiesV1*)>::value, "scoped property getScopedCapabilities exact signature");
static_assert(std::is_same<decltype(((MmsUiPropertyRefreshApiV1_1*)nullptr)->subscribeScoped), MmsResult (MMS_CALL *)(void*, const MmsUiPropertyRefreshScopedSubscriptionDescV1*, MmsHandle*)>::value, "scoped property subscribeScoped exact signature");
static_assert(std::is_same<decltype(((MmsUiPropertyRefreshApiV1_1*)nullptr)->queryScoped), MmsResult (MMS_CALL *)(void*, MmsHandle, MmsUiPropertyRefreshScopedSubscriptionStateV1*)>::value, "scoped property queryScoped exact signature");

#include <mms/evaluation.h>
static_assert(sizeof(MmsEvaluationCapsV1) == 40, "MmsEvaluationCapsV1 layout");
static_assert(std::is_standard_layout<MmsEvaluationCapsV1>::value && std::is_trivially_copyable<MmsEvaluationCapsV1>::value, "MmsEvaluationCapsV1 POD");
static_assert(sizeof(MmsEvaluationSourceQueryV1) == 48, "MmsEvaluationSourceQueryV1 layout");
static_assert(std::is_standard_layout<MmsEvaluationSourceQueryV1>::value && std::is_trivially_copyable<MmsEvaluationSourceQueryV1>::value, "MmsEvaluationSourceQueryV1 POD");
static_assert(sizeof(MmsEvaluationSourceInfoV1) == 64, "MmsEvaluationSourceInfoV1 layout");
static_assert(std::is_standard_layout<MmsEvaluationSourceInfoV1>::value && std::is_trivially_copyable<MmsEvaluationSourceInfoV1>::value, "MmsEvaluationSourceInfoV1 POD");
static_assert(sizeof(MmsEvaluationBoneV1) == 32, "MmsEvaluationBoneV1 layout");
static_assert(std::is_standard_layout<MmsEvaluationBoneV1>::value && std::is_trivially_copyable<MmsEvaluationBoneV1>::value, "MmsEvaluationBoneV1 POD");
static_assert(sizeof(MmsBoneChannelsV1) == 80, "MmsBoneChannelsV1 layout");
static_assert(std::is_standard_layout<MmsBoneChannelsV1>::value && std::is_trivially_copyable<MmsBoneChannelsV1>::value, "MmsBoneChannelsV1 POD");
static_assert(sizeof(MmsEvaluationApiV1) == 72, "MmsEvaluationApiV1 layout");
static_assert(std::is_standard_layout<MmsEvaluationApiV1>::value && std::is_trivially_copyable<MmsEvaluationApiV1>::value, "MmsEvaluationApiV1 POD");
static_assert(offsetof(MmsEvaluationApiV1, context) == 8, "evaluation context offset");
static_assert(offsetof(MmsEvaluationApiV1, getCapabilities) == 16, "evaluation getCapabilities offset");
static_assert(offsetof(MmsEvaluationApiV1, createSource) == 24, "evaluation createSource offset");
static_assert(offsetof(MmsEvaluationApiV1, describeSource) == 32, "evaluation describeSource offset");
static_assert(offsetof(MmsEvaluationApiV1, boneAt) == 40, "evaluation boneAt offset");
static_assert(offsetof(MmsEvaluationApiV1, sampleBone) == 48, "evaluation sampleBone offset");
static_assert(offsetof(MmsEvaluationApiV1, retain) == 56, "evaluation retain offset");
static_assert(offsetof(MmsEvaluationApiV1, release) == 64, "evaluation release offset");
static_assert(std::is_same<decltype(MmsEvaluationApiV1::sampleBone), MmsResult (MMS_CALL *)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*)>::value, "evaluation sample signature");
