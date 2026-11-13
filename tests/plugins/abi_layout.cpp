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
#include <mms/layout_session.h>
static_assert(offsetof(MmsUiLayoutSessionApiV1, queryCapabilities) == 16, "session capabilities offset");
static_assert(offsetof(MmsUiLayoutSessionApiV1, release) == 80, "session release offset");
static_assert(offsetof(MmsUiLayoutSessionValueV1, width) == 24, "session dimensions offset");
static_assert(offsetof(MmsUiLayoutSessionInfoV1, revision) == 8, "session revision offset");
static_assert(std::is_standard_layout<MmsUiLayoutSessionApiV1>::value, "session standard layout");
static_assert(std::is_standard_layout<MmsUiLayoutSessionApiV1_2>::value, "parent session standard layout");
static_assert(offsetof(MmsUiLayoutSessionApiV1_2, queryCapabilitiesEx) == sizeof(MmsUiLayoutSessionApiV1), "parent session appended functions");
static_assert(offsetof(MmsUiLayoutSessionApiV1_2, queryClaimEx) == 120, "parent session final function");
static_assert(offsetof(MmsUiLayoutSessionTargetV1_2, parent) == sizeof(MmsUiLayoutSessionTargetV1), "parent claim append");
static_assert(offsetof(MmsUiLayoutSessionValueV1_2, parent) == sizeof(MmsUiLayoutSessionValueV1), "parent value append");
#define MMS_CHECK_SESSION_PREFIX(member) static_assert(offsetof(MmsUiLayoutSessionApiV1_2, member) == offsetof(MmsUiLayoutSessionApiV1, member), "session prefix " #member)
MMS_CHECK_SESSION_PREFIX(h);
MMS_CHECK_SESSION_PREFIX(context);
MMS_CHECK_SESSION_PREFIX(queryCapabilities);
MMS_CHECK_SESSION_PREFIX(queryTarget);
MMS_CHECK_SESSION_PREFIX(begin);
MMS_CHECK_SESSION_PREFIX(apply);
MMS_CHECK_SESSION_PREFIX(query);
MMS_CHECK_SESSION_PREFIX(queryClaim);
MMS_CHECK_SESSION_PREFIX(rollback);
MMS_CHECK_SESSION_PREFIX(commit);
MMS_CHECK_SESSION_PREFIX(release);
#undef MMS_CHECK_SESSION_PREFIX
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

static_assert(sizeof(MmsEvaluationSourceInfoV1_1) == 72, "MmsEvaluationSourceInfoV1_1 layout");
static_assert(std::is_standard_layout<MmsEvaluationSourceInfoV1_1>::value && std::is_trivially_copyable<MmsEvaluationSourceInfoV1_1>::value, "MmsEvaluationSourceInfoV1_1 POD");
static_assert(sizeof(MmsEvaluationMorphV1) == 32, "MmsEvaluationMorphV1 layout");
static_assert(std::is_standard_layout<MmsEvaluationMorphV1>::value && std::is_trivially_copyable<MmsEvaluationMorphV1>::value, "MmsEvaluationMorphV1 POD");
static_assert(sizeof(MmsMorphWeightV1) == 16, "MmsMorphWeightV1 layout");
static_assert(std::is_standard_layout<MmsMorphWeightV1>::value && std::is_trivially_copyable<MmsMorphWeightV1>::value, "MmsMorphWeightV1 POD");
static_assert(sizeof(MmsEvaluationApiV1_1) == 104, "MmsEvaluationApiV1_1 layout");
static_assert(std::is_standard_layout<MmsEvaluationApiV1_1>::value && std::is_trivially_copyable<MmsEvaluationApiV1_1>::value, "MmsEvaluationApiV1_1 POD");
static_assert(offsetof(MmsEvaluationApiV1_1, context) == 8, "evaluation1.1 context offset");
static_assert(offsetof(MmsEvaluationApiV1_1, getCapabilities) == 16, "evaluation1.1 getCapabilities offset");
static_assert(offsetof(MmsEvaluationApiV1_1, createSource) == 24, "evaluation1.1 createSource offset");
static_assert(offsetof(MmsEvaluationApiV1_1, describeSource) == 32, "evaluation1.1 describeSource offset");
static_assert(offsetof(MmsEvaluationApiV1_1, boneAt) == 40, "evaluation1.1 boneAt offset");
static_assert(offsetof(MmsEvaluationApiV1_1, sampleBone) == 48, "evaluation1.1 sampleBone offset");
static_assert(offsetof(MmsEvaluationApiV1_1, retain) == 56, "evaluation1.1 retain offset");
static_assert(offsetof(MmsEvaluationApiV1_1, release) == 64, "evaluation1.1 release offset");
static_assert(offsetof(MmsEvaluationApiV1_1, createSourceChannels) == 72, "evaluation1.1 createSourceChannels offset");
static_assert(offsetof(MmsEvaluationApiV1_1, describeSourceChannels) == 80, "evaluation1.1 describeSourceChannels offset");
static_assert(offsetof(MmsEvaluationApiV1_1, morphAt) == 88, "evaluation1.1 morphAt offset");
static_assert(offsetof(MmsEvaluationApiV1_1, sampleMorph) == 96, "evaluation1.1 sampleMorph offset");
static_assert(offsetof(MmsEvaluationSourceInfoV1_1, morphCount) == 64, "extended source prefix");

static_assert(sizeof(MmsEvaluationSourceInfoV1_2) == 80, "MmsEvaluationSourceInfoV1_2 layout");
static_assert(sizeof(MmsEvaluationIkV1) == 24, "MmsEvaluationIkV1 layout");
static_assert(sizeof(MmsEvaluationSelectorV1) == 32, "MmsEvaluationSelectorV1 layout");
static_assert(sizeof(MmsDisplayStateV1) == 16, "MmsDisplayStateV1 layout");
static_assert(sizeof(MmsIkStateV1) == 16, "MmsIkStateV1 layout");
static_assert(sizeof(MmsParentSelectionV1) == 64, "MmsParentSelectionV1 layout");
static_assert(sizeof(MmsEvaluationApiV1_2) == 152, "MmsEvaluationApiV1_2 layout");
static_assert(offsetof(MmsEvaluationApiV1_2, context) == 8, "evaluation1.2 context offset");
static_assert(offsetof(MmsEvaluationApiV1_2, getCapabilities) == 16, "evaluation1.2 getCapabilities offset");
static_assert(offsetof(MmsEvaluationApiV1_2, createSource) == 24, "evaluation1.2 createSource offset");
static_assert(offsetof(MmsEvaluationApiV1_2, describeSource) == 32, "evaluation1.2 describeSource offset");
static_assert(offsetof(MmsEvaluationApiV1_2, boneAt) == 40, "evaluation1.2 boneAt offset");
static_assert(offsetof(MmsEvaluationApiV1_2, sampleBone) == 48, "evaluation1.2 sampleBone offset");
static_assert(offsetof(MmsEvaluationApiV1_2, retain) == 56, "evaluation1.2 retain offset");
static_assert(offsetof(MmsEvaluationApiV1_2, release) == 64, "evaluation1.2 release offset");
static_assert(offsetof(MmsEvaluationApiV1_2, createSourceChannels) == 72, "evaluation1.2 createSourceChannels offset");
static_assert(offsetof(MmsEvaluationApiV1_2, describeSourceChannels) == 80, "evaluation1.2 describeSourceChannels offset");
static_assert(offsetof(MmsEvaluationApiV1_2, morphAt) == 88, "evaluation1.2 morphAt offset");
static_assert(offsetof(MmsEvaluationApiV1_2, sampleMorph) == 96, "evaluation1.2 sampleMorph offset");
static_assert(offsetof(MmsEvaluationApiV1_2, describeSourceDisplay) == 104, "evaluation1.2 describeSourceDisplay offset");
static_assert(offsetof(MmsEvaluationApiV1_2, ikAt) == 112, "evaluation1.2 ikAt offset");
static_assert(offsetof(MmsEvaluationApiV1_2, selectorAt) == 120, "evaluation1.2 selectorAt offset");
static_assert(offsetof(MmsEvaluationApiV1_2, sampleVisibility) == 128, "evaluation1.2 sampleVisibility offset");
static_assert(offsetof(MmsEvaluationApiV1_2, sampleIk) == 136, "evaluation1.2 sampleIk offset");
static_assert(offsetof(MmsEvaluationApiV1_2, sampleParent) == 144, "evaluation1.2 sampleParent offset");
static_assert(offsetof(MmsEvaluationSourceInfoV1_2, ikCount) == 72, "evaluation1.2 source prefix");

static_assert(sizeof(MmsParentStateV1) == 64, "parent state layout");
static_assert(sizeof(MmsEvaluationApiV1_3) == 160, "evaluation1.3 layout");
static_assert(offsetof(MmsEvaluationApiV1_3, sampleParentState) == sizeof(MmsEvaluationApiV1_2), "evaluation1.3 preserves prefix");
static_assert(offsetof(MmsParentStateV1, h) == 0, "parent state h offset");
static_assert(offsetof(MmsParentStateV1, target) == 8, "parent state target offset");
static_assert(offsetof(MmsParentStateV1, bone) == 32, "parent state bone offset");
static_assert(offsetof(MmsParentStateV1, windowStart) == 48, "parent state windowStart offset");
static_assert(offsetof(MmsParentStateV1, windowEnd) == 52, "parent state windowEnd offset");
static_assert(offsetof(MmsParentStateV1, kind) == 56, "parent state kind offset");
static_assert(offsetof(MmsParentStateV1, reserved) == 60, "parent state reserved offset");
static_assert(std::is_standard_layout<MmsParentStateV1>::value && std::is_trivially_copyable<MmsParentStateV1>::value, "MmsParentStateV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationApiV1_3>::value && std::is_trivially_copyable<MmsEvaluationApiV1_3>::value, "MmsEvaluationApiV1_3 POD");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_3::sampleParentState), MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*)>::value, "parent state sample signature");

static_assert(sizeof(MmsEvaluationSceneCapsV1) == 72, "MmsEvaluationSceneCapsV1 layout");
static_assert(sizeof(MmsEvaluationSceneQueryV1) == 24, "MmsEvaluationSceneQueryV1 layout");
static_assert(sizeof(MmsEvaluationSceneInfoV1) == 40, "MmsEvaluationSceneInfoV1 layout");
static_assert(sizeof(MmsEvaluationSceneModelV1) == 48, "MmsEvaluationSceneModelV1 layout");
static_assert(sizeof(MmsEvaluationSceneBoneV1) == 32, "MmsEvaluationSceneBoneV1 layout");
static_assert(sizeof(MmsEvaluationFrameQueryV1) == 56, "MmsEvaluationFrameQueryV1 layout");
static_assert(sizeof(MmsEvaluationFrameInfoV1) == 64, "MmsEvaluationFrameInfoV1 layout");
static_assert(sizeof(MmsEvaluationScenePoseV1) == 296, "MmsEvaluationScenePoseV1 layout");
static_assert(sizeof(MmsEvaluationApiV1_4) == 232, "MmsEvaluationApiV1_4 layout");
static_assert(offsetof(MmsEvaluationApiV1_4, context) == offsetof(MmsEvaluationApiV1_3, context), "evaluation1.4 context prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, getCapabilities) == offsetof(MmsEvaluationApiV1_3, getCapabilities), "evaluation1.4 getCapabilities prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, createSource) == offsetof(MmsEvaluationApiV1_3, createSource), "evaluation1.4 createSource prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, describeSource) == offsetof(MmsEvaluationApiV1_3, describeSource), "evaluation1.4 describeSource prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, boneAt) == offsetof(MmsEvaluationApiV1_3, boneAt), "evaluation1.4 boneAt prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, sampleBone) == offsetof(MmsEvaluationApiV1_3, sampleBone), "evaluation1.4 sampleBone prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, retain) == offsetof(MmsEvaluationApiV1_3, retain), "evaluation1.4 retain prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, release) == offsetof(MmsEvaluationApiV1_3, release), "evaluation1.4 release prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, createSourceChannels) == offsetof(MmsEvaluationApiV1_3, createSourceChannels), "evaluation1.4 createSourceChannels prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, describeSourceChannels) == offsetof(MmsEvaluationApiV1_3, describeSourceChannels), "evaluation1.4 describeSourceChannels prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, morphAt) == offsetof(MmsEvaluationApiV1_3, morphAt), "evaluation1.4 morphAt prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, sampleMorph) == offsetof(MmsEvaluationApiV1_3, sampleMorph), "evaluation1.4 sampleMorph prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, describeSourceDisplay) == offsetof(MmsEvaluationApiV1_3, describeSourceDisplay), "evaluation1.4 describeSourceDisplay prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, ikAt) == offsetof(MmsEvaluationApiV1_3, ikAt), "evaluation1.4 ikAt prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, selectorAt) == offsetof(MmsEvaluationApiV1_3, selectorAt), "evaluation1.4 selectorAt prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, sampleVisibility) == offsetof(MmsEvaluationApiV1_3, sampleVisibility), "evaluation1.4 sampleVisibility prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, sampleIk) == offsetof(MmsEvaluationApiV1_3, sampleIk), "evaluation1.4 sampleIk prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, sampleParent) == offsetof(MmsEvaluationApiV1_3, sampleParent), "evaluation1.4 sampleParent prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, sampleParentState) == offsetof(MmsEvaluationApiV1_3, sampleParentState), "evaluation1.4 sampleParentState prefix");
static_assert(offsetof(MmsEvaluationApiV1_4, getSceneCapabilities) == sizeof(MmsEvaluationApiV1_3), "evaluation1.4 extension offset");
static_assert(std::is_standard_layout<MmsEvaluationSceneCapsV1>::value && std::is_trivially_copyable<MmsEvaluationSceneCapsV1>::value, "MmsEvaluationSceneCapsV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationSceneQueryV1>::value && std::is_trivially_copyable<MmsEvaluationSceneQueryV1>::value, "MmsEvaluationSceneQueryV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationSceneInfoV1>::value && std::is_trivially_copyable<MmsEvaluationSceneInfoV1>::value, "MmsEvaluationSceneInfoV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationSceneModelV1>::value && std::is_trivially_copyable<MmsEvaluationSceneModelV1>::value, "MmsEvaluationSceneModelV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationSceneBoneV1>::value && std::is_trivially_copyable<MmsEvaluationSceneBoneV1>::value, "MmsEvaluationSceneBoneV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationFrameQueryV1>::value && std::is_trivially_copyable<MmsEvaluationFrameQueryV1>::value, "MmsEvaluationFrameQueryV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationFrameInfoV1>::value && std::is_trivially_copyable<MmsEvaluationFrameInfoV1>::value, "MmsEvaluationFrameInfoV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationScenePoseV1>::value && std::is_trivially_copyable<MmsEvaluationScenePoseV1>::value, "MmsEvaluationScenePoseV1 POD");
static_assert(std::is_standard_layout<MmsEvaluationApiV1_4>::value && std::is_trivially_copyable<MmsEvaluationApiV1_4>::value, "MmsEvaluationApiV1_4 POD");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_4::readBonePose), MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationScenePoseV1*)>::value, "scene pose signature");

static_assert(sizeof(MmsEvaluationMeshCapsV1) == 24, "mesh evaluation capabilities layout");
static_assert(sizeof(MmsEvaluationMeshInfoV1) == 56, "mesh evaluation information layout");
static_assert(sizeof(MmsEvaluationMeshMaterialV1) == 24, "mesh evaluation material layout");
static_assert(sizeof(MmsEvaluationMeshVertexV1) == 112, "mesh evaluation vertex layout");
static_assert(offsetof(MmsEvaluationMeshInfoV1, object) == 8, "mesh identity offset");
static_assert(offsetof(MmsEvaluationMeshInfoV1, vertexCount) == 32, "mesh counts offset");
static_assert(offsetof(MmsEvaluationMeshVertexV1, position) == 8, "mesh position offset");
static_assert(offsetof(MmsEvaluationMeshVertexV1, normal) == 20, "mesh normal offset");
static_assert(offsetof(MmsEvaluationMeshVertexV1, uv) == 32, "mesh UV offset");
static_assert(offsetof(MmsEvaluationMeshVertexV1, additionalUv) == 40, "mesh additional UV offset");
static_assert(offsetof(MmsEvaluationMeshVertexV1, reserved) == 104, "mesh reserved offset");
static_assert(sizeof(MmsEvaluationApiV1_5) == 288, "MmsEvaluationApiV1_5 layout");
#define MMS_EVALUATION_15_PREFIX(member) \
    static_assert(offsetof(MmsEvaluationApiV1_5, member) == offsetof(MmsEvaluationApiV1_4, member), "evaluation1.5 " #member " offset"); \
    static_assert(std::is_same<decltype(MmsEvaluationApiV1_5::member), decltype(MmsEvaluationApiV1_4::member)>::value, "evaluation1.5 " #member " type")
MMS_EVALUATION_15_PREFIX(h);
MMS_EVALUATION_15_PREFIX(context);
MMS_EVALUATION_15_PREFIX(getCapabilities);
MMS_EVALUATION_15_PREFIX(createSource);
MMS_EVALUATION_15_PREFIX(describeSource);
MMS_EVALUATION_15_PREFIX(boneAt);
MMS_EVALUATION_15_PREFIX(sampleBone);
MMS_EVALUATION_15_PREFIX(retain);
MMS_EVALUATION_15_PREFIX(release);
MMS_EVALUATION_15_PREFIX(createSourceChannels);
MMS_EVALUATION_15_PREFIX(describeSourceChannels);
MMS_EVALUATION_15_PREFIX(morphAt);
MMS_EVALUATION_15_PREFIX(sampleMorph);
MMS_EVALUATION_15_PREFIX(describeSourceDisplay);
MMS_EVALUATION_15_PREFIX(ikAt);
MMS_EVALUATION_15_PREFIX(selectorAt);
MMS_EVALUATION_15_PREFIX(sampleVisibility);
MMS_EVALUATION_15_PREFIX(sampleIk);
MMS_EVALUATION_15_PREFIX(sampleParent);
MMS_EVALUATION_15_PREFIX(sampleParentState);
MMS_EVALUATION_15_PREFIX(getSceneCapabilities);
MMS_EVALUATION_15_PREFIX(createSceneSource);
MMS_EVALUATION_15_PREFIX(describeSceneSource);
MMS_EVALUATION_15_PREFIX(sceneModelAt);
MMS_EVALUATION_15_PREFIX(sceneBoneAt);
MMS_EVALUATION_15_PREFIX(evaluateFrame);
MMS_EVALUATION_15_PREFIX(describeFrame);
MMS_EVALUATION_15_PREFIX(frameModelAt);
MMS_EVALUATION_15_PREFIX(readBonePose);
#undef MMS_EVALUATION_15_PREFIX
static_assert(offsetof(MmsEvaluationApiV1_5, getMeshCapabilities) == sizeof(MmsEvaluationApiV1_4), "evaluation1.5 extension offset");
static_assert(offsetof(MmsEvaluationApiV1_5, createMeshSceneSource) == 240, "mesh scene create offset");
static_assert(offsetof(MmsEvaluationApiV1_5, describeMesh) == 248, "mesh info offset");
static_assert(offsetof(MmsEvaluationApiV1_5, readMeshIndices) == 256, "mesh indices offset");
static_assert(offsetof(MmsEvaluationApiV1_5, readMeshMaterials) == 264, "mesh materials offset");
static_assert(offsetof(MmsEvaluationApiV1_5, readMeshBindVertices) == 272, "mesh bind vertex offset");
static_assert(offsetof(MmsEvaluationApiV1_5, readMeshVertices) == 280, "mesh evaluated vertex offset");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_5::createMeshSceneSource), MmsResult (MMS_CALL *)(void*, const MmsEvaluationSceneQueryV1*, MmsHandle*)>::value, "mesh source signature");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_5::readMeshIndices), MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, uint32_t, uint32_t, uint32_t*)>::value, "mesh indices signature");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_5::readMeshMaterials), MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, uint32_t, uint32_t, MmsEvaluationMeshMaterialV1*)>::value, "mesh materials signature");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_5::readMeshBindVertices), MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, uint32_t, uint32_t, MmsEvaluationMeshVertexV1*)>::value, "mesh bind vertex signature");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_5::readMeshVertices), MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, uint32_t, uint32_t, MmsEvaluationMeshVertexV1*)>::value, "mesh evaluated vertex signature");
static_assert(std::is_standard_layout<MmsEvaluationMeshCapsV1>::value && std::is_trivially_copyable<MmsEvaluationMeshCapsV1>::value, "mesh capabilities POD");
static_assert(std::is_standard_layout<MmsEvaluationMeshInfoV1>::value && std::is_trivially_copyable<MmsEvaluationMeshInfoV1>::value, "mesh information POD");
static_assert(std::is_standard_layout<MmsEvaluationMeshMaterialV1>::value && std::is_trivially_copyable<MmsEvaluationMeshMaterialV1>::value, "mesh material POD");
static_assert(std::is_standard_layout<MmsEvaluationMeshVertexV1>::value && std::is_trivially_copyable<MmsEvaluationMeshVertexV1>::value, "mesh vertex POD");
static_assert(std::is_standard_layout<MmsEvaluationApiV1_5>::value && std::is_trivially_copyable<MmsEvaluationApiV1_5>::value, "evaluation 1.5 POD");

static_assert(sizeof(MmsEvaluationMaterialCapsV1)==24, "material capabilities layout");
static_assert(sizeof(MmsEvaluationMaterialIdV1)==32, "material identity layout");
static_assert(sizeof(MmsEvaluationMaterialV1)==216, "evaluated material layout");
static_assert(offsetof(MmsEvaluationMaterialIdV1,material)==8 && offsetof(MmsEvaluationMaterialIdV1,index)==24, "material identity offsets");
static_assert(offsetof(MmsEvaluationMaterialV1,material)==8 && offsetof(MmsEvaluationMaterialV1,index)==24 && offsetof(MmsEvaluationMaterialV1,flags)==28, "material identity and flags offsets");
static_assert(offsetof(MmsEvaluationMaterialV1,nativeDiffuse)==32 && offsetof(MmsEvaluationMaterialV1,diffuseMirrorAndAlpha)==44, "material diffuse offsets");
static_assert(offsetof(MmsEvaluationMaterialV1,specular)==60 && offsetof(MmsEvaluationMaterialV1,ambient)==72 && offsetof(MmsEvaluationMaterialV1,specularPower)==84, "material lighting offsets");
static_assert(offsetof(MmsEvaluationMaterialV1,edgeColor)==88 && offsetof(MmsEvaluationMaterialV1,edgeSize)==104, "material edge offsets");
static_assert(offsetof(MmsEvaluationMaterialV1,textureAdd)==108 && offsetof(MmsEvaluationMaterialV1,textureMultiply)==124, "texture tint offsets");
static_assert(offsetof(MmsEvaluationMaterialV1,sphereAdd)==140 && offsetof(MmsEvaluationMaterialV1,sphereMultiply)==156, "sphere tint offsets");
static_assert(offsetof(MmsEvaluationMaterialV1,toonAdd)==172 && offsetof(MmsEvaluationMaterialV1,toonMultiply)==188 && offsetof(MmsEvaluationMaterialV1,reserved)==204, "toon tint offsets");
static_assert(sizeof(MmsEvaluationApiV1_6)==320, "MmsEvaluationApiV1_6 layout");
#define MMS_EVALUATION_16_PREFIX(member) \
    static_assert(offsetof(MmsEvaluationApiV1_6, member)==offsetof(MmsEvaluationApiV1_5, member), "evaluation1.6 " #member " offset"); \
    static_assert(std::is_same<decltype(MmsEvaluationApiV1_6::member),decltype(MmsEvaluationApiV1_5::member)>::value, "evaluation1.6 " #member " type")
MMS_EVALUATION_16_PREFIX(h);
MMS_EVALUATION_16_PREFIX(context);
MMS_EVALUATION_16_PREFIX(getCapabilities);
MMS_EVALUATION_16_PREFIX(createSource);
MMS_EVALUATION_16_PREFIX(describeSource);
MMS_EVALUATION_16_PREFIX(boneAt);
MMS_EVALUATION_16_PREFIX(sampleBone);
MMS_EVALUATION_16_PREFIX(retain);
MMS_EVALUATION_16_PREFIX(release);
MMS_EVALUATION_16_PREFIX(createSourceChannels);
MMS_EVALUATION_16_PREFIX(describeSourceChannels);
MMS_EVALUATION_16_PREFIX(morphAt);
MMS_EVALUATION_16_PREFIX(sampleMorph);
MMS_EVALUATION_16_PREFIX(describeSourceDisplay);
MMS_EVALUATION_16_PREFIX(ikAt);
MMS_EVALUATION_16_PREFIX(selectorAt);
MMS_EVALUATION_16_PREFIX(sampleVisibility);
MMS_EVALUATION_16_PREFIX(sampleIk);
MMS_EVALUATION_16_PREFIX(sampleParent);
MMS_EVALUATION_16_PREFIX(sampleParentState);
MMS_EVALUATION_16_PREFIX(getSceneCapabilities);
MMS_EVALUATION_16_PREFIX(createSceneSource);
MMS_EVALUATION_16_PREFIX(describeSceneSource);
MMS_EVALUATION_16_PREFIX(sceneModelAt);
MMS_EVALUATION_16_PREFIX(sceneBoneAt);
MMS_EVALUATION_16_PREFIX(evaluateFrame);
MMS_EVALUATION_16_PREFIX(describeFrame);
MMS_EVALUATION_16_PREFIX(frameModelAt);
MMS_EVALUATION_16_PREFIX(readBonePose);
MMS_EVALUATION_16_PREFIX(getMeshCapabilities);
MMS_EVALUATION_16_PREFIX(createMeshSceneSource);
MMS_EVALUATION_16_PREFIX(describeMesh);
MMS_EVALUATION_16_PREFIX(readMeshIndices);
MMS_EVALUATION_16_PREFIX(readMeshMaterials);
MMS_EVALUATION_16_PREFIX(readMeshBindVertices);
MMS_EVALUATION_16_PREFIX(readMeshVertices);
#undef MMS_EVALUATION_16_PREFIX
static_assert(offsetof(MmsEvaluationApiV1_6,getMaterialCapabilities)==sizeof(MmsEvaluationApiV1_5), "material capabilities extension offset");
static_assert(offsetof(MmsEvaluationApiV1_6,createMaterialSceneSource)==296, "material source offset");
static_assert(offsetof(MmsEvaluationApiV1_6,materialAt)==304, "material identity function offset");
static_assert(offsetof(MmsEvaluationApiV1_6,readFrameMaterials)==312, "material frame reader offset");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_6::getMaterialCapabilities),MmsResult (MMS_CALL *)(void*,MmsEvaluationMaterialCapsV1*)>::value, "material caps signature");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_6::createMaterialSceneSource),MmsResult (MMS_CALL *)(void*,const MmsEvaluationSceneQueryV1*,MmsHandle*)>::value, "material source signature");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_6::materialAt),MmsResult (MMS_CALL *)(void*,MmsHandle,uint32_t,uint32_t,MmsEvaluationMaterialIdV1*)>::value, "material identity signature");
static_assert(std::is_same<decltype(MmsEvaluationApiV1_6::readFrameMaterials),MmsResult (MMS_CALL *)(void*,MmsHandle,uint32_t,uint32_t,uint32_t,MmsEvaluationMaterialV1*)>::value, "material reader signature");
static_assert(std::is_standard_layout<MmsEvaluationMaterialCapsV1>::value && std::is_trivially_copyable<MmsEvaluationMaterialCapsV1>::value, "material capabilities POD");
static_assert(std::is_standard_layout<MmsEvaluationMaterialIdV1>::value && std::is_trivially_copyable<MmsEvaluationMaterialIdV1>::value, "material identity POD");
static_assert(std::is_standard_layout<MmsEvaluationMaterialV1>::value && std::is_trivially_copyable<MmsEvaluationMaterialV1>::value, "evaluated material POD");
static_assert(std::is_standard_layout<MmsEvaluationApiV1_6>::value && std::is_trivially_copyable<MmsEvaluationApiV1_6>::value, "evaluation 1.6 POD");
static_assert(sizeof(MmsEvaluationReferenceCapsV1)==16, "reference capabilities layout");
static_assert(sizeof(MmsEvaluationMaterialReferencesV1)==2184, "material references layout");
static_assert(sizeof(MmsEvaluationToonTableV1)==1520, "toon table layout");
static_assert(offsetof(MmsEvaluationApiV1_7,getReferenceCapabilities)==sizeof(MmsEvaluationApiV1_6), "evaluation 1.6 prefix");
static_assert(sizeof(MmsEvaluationApiV1_7)==sizeof(MmsEvaluationApiV1_6)+4*sizeof(void*), "evaluation 1.7 append");
static_assert(std::is_standard_layout<MmsEvaluationApiV1_7>::value && std::is_trivially_copyable<MmsEvaluationApiV1_7>::value, "evaluation 1.7 POD");
