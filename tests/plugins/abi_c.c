#include <mms/surface.h>
#include <mms/refresh.h>
#include <mms/refresh_binding.h>
#include <mms/refresh_state.h>
#include <mms/refresh_dependency.h>
#include <mms/refresh_subscription.h>
#include <mms/property_refresh.h>
#include <mms/render_image.h>
_Static_assert(sizeof(MmsRenderImageCapsV1) == 56, "image capabilities");
_Static_assert(sizeof(MmsRenderImageDescV1) == 40, "image descriptor");
_Static_assert(sizeof(MmsRenderImageInfoV1) == 56, "image info");
_Static_assert(sizeof(MmsRenderImageMapV1) == 40, "image mapping");
_Static_assert(sizeof(MmsRenderImageApiV1) == 72, "image API");
_Static_assert(offsetof(MmsRenderImageDescV1, rowStride) == 32, "image stride offset");
_Static_assert(offsetof(MmsRenderImageApiV1, release) == 64, "image release offset");
_Static_assert(sizeof(MmsUiRefreshSubscriptionCapabilitiesV1) == 32, "subscription capabilities");
_Static_assert(sizeof(MmsUiRefreshSubscriptionDescV1) == 32, "subscription descriptor");
_Static_assert(sizeof(MmsUiRefreshSubscriptionStateV1) == 72, "subscription state");
_Static_assert(sizeof(MmsUiRefreshSubscriptionApiV1) == 48, "subscription API");
_Static_assert(offsetof(MmsUiRefreshSubscriptionDescV1, binding) == 8, "subscription binding offset");
_Static_assert(offsetof(MmsUiRefreshSubscriptionStateV1, lastResult) == 56, "subscription result offset");
_Static_assert(offsetof(MmsUiRefreshSubscriptionApiV1, unsubscribe) == 40, "subscription unsubscribe offset");
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

/* New original-property refresh ABI: x64 pack(8) binary layout. */
_Static_assert(sizeof(MmsUiPropertyRefV1) == 32, "property reference");
_Static_assert(offsetof(MmsUiPropertyRefV1, h) == 0, "property reference header offset");
_Static_assert(offsetof(MmsUiPropertyRefV1, property) == 8, "property reference property offset");
_Static_assert(offsetof(MmsUiPropertyRefV1, registrationGeneration) == 16, "property reference registration offset");
_Static_assert(offsetof(MmsUiPropertyRefV1, projectEpoch) == 24, "property reference epoch offset");
_Static_assert(sizeof(MmsUiPropertyRefreshCapabilitiesV1) == 32, "property refresh capabilities");
_Static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, h) == 0, "property refresh capabilities header offset");
_Static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, maxSubscriptionsPerPlugin) == 8, "property refresh capabilities count offset");
_Static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, supportedKinds) == 12, "property refresh capabilities kinds offset");
_Static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, supportedFlags) == 16, "property refresh capabilities flags offset");
_Static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, flags) == 20, "property refresh capabilities feature offset");
_Static_assert(offsetof(MmsUiPropertyRefreshCapabilitiesV1, reserved) == 24, "property refresh capabilities reserved offset");
_Static_assert(sizeof(MmsUiPropertyRefreshSubscriptionDescV1) == 88, "property refresh descriptor");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, h) == 0, "property refresh descriptor header offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, target) == 8, "property refresh descriptor target offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, property) == 40, "property refresh descriptor property offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, kinds) == 72, "property refresh descriptor kinds offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, flags) == 76, "property refresh descriptor flags offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionDescV1, reserved) == 80, "property refresh descriptor reserved offset");
_Static_assert(sizeof(MmsUiPropertyRefreshSubscriptionStateV1) == 120, "property refresh state");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, h) == 0, "property refresh state header offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, subscription) == 8, "property refresh state subscription offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, target) == 16, "property refresh state target offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, property) == 48, "property refresh state property offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, latestSequence) == 80, "property refresh state latest offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, submittedSequence) == 88, "property refresh state submitted offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, completedSequence) == 96, "property refresh state completed offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, lastResult) == 104, "property refresh state result offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, pending) == 108, "property refresh state pending offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, waitingFor) == 112, "property refresh state waiting offset");
_Static_assert(offsetof(MmsUiPropertyRefreshSubscriptionStateV1, reserved) == 116, "property refresh state reserved offset");
_Static_assert(sizeof(MmsUiPropertyRefreshApiV1) == 64, "property refresh API");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, h) == 0, "property refresh API header offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, context) == 8, "property refresh API context offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, getCapabilities) == 16, "property refresh API getCapabilities offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, getPropertyRef) == 24, "property refresh API getPropertyRef offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, subscribe) == 32, "property refresh API subscribe offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, notify) == 40, "property refresh API notify offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, query) == 48, "property refresh API query offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1, unsubscribe) == 56, "property refresh API unsubscribe offset");

_Static_assert(sizeof(MmsUiRefreshScopedSubscriptionDescV1) == 80, "scoped subscription descriptor");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, binding) == 8, "scoped trigger binding");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, seed) == 16, "scoped seed reference");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, scope) == 48, "scoped scope");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, kinds) == 52, "scoped kinds");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, flags) == 56, "scoped flags");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, reserved) == 60, "scoped reserved");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, reservedTail) == 64, "scoped descriptor tail");
_Static_assert(sizeof(MmsUiRefreshScopedSubscriptionStateV1) == 160, "scoped subscription state");
#define MMS_SCOPED_STATE_PREFIX(field) _Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, field) == offsetof(MmsUiRefreshSubscriptionStateV1, field), "scoped state prefix " #field)
MMS_SCOPED_STATE_PREFIX(h); MMS_SCOPED_STATE_PREFIX(subscription); MMS_SCOPED_STATE_PREFIX(binding);
MMS_SCOPED_STATE_PREFIX(projectEpoch); MMS_SCOPED_STATE_PREFIX(latestRevision); MMS_SCOPED_STATE_PREFIX(submittedRevision);
MMS_SCOPED_STATE_PREFIX(completedRevision); MMS_SCOPED_STATE_PREFIX(lastResult); MMS_SCOPED_STATE_PREFIX(pending); MMS_SCOPED_STATE_PREFIX(reserved);
#undef MMS_SCOPED_STATE_PREFIX
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, seed) == 72, "scoped state seed");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, scope) == 104, "scoped state scope");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, kinds) == 108, "scoped state kinds");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, workGeneration) == 112, "scoped latest work");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, submittedWorkGeneration) == 120, "scoped submitted work");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, completedWorkGeneration) == 128, "scoped completed work");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, submittedDependencyRevision) == 136, "scoped graph revision");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, waitingFor) == 144, "scoped wait reason");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, flags) == 148, "scoped state flags");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionStateV1, reservedTail) == 152, "scoped state reserved tail");
_Static_assert(sizeof(MmsUiRefreshSubscriptionApiV1_1) == 64, "scoped subscription API");
#define MMS_SCOPED_API_PREFIX(field) _Static_assert(offsetof(MmsUiRefreshSubscriptionApiV1_1, field) == offsetof(MmsUiRefreshSubscriptionApiV1, field), "scoped API prefix " #field)
MMS_SCOPED_API_PREFIX(h); MMS_SCOPED_API_PREFIX(context); MMS_SCOPED_API_PREFIX(getCapabilities);
MMS_SCOPED_API_PREFIX(subscribe); MMS_SCOPED_API_PREFIX(query); MMS_SCOPED_API_PREFIX(unsubscribe);
#undef MMS_SCOPED_API_PREFIX
_Static_assert(offsetof(MmsUiRefreshSubscriptionApiV1_1, subscribeScoped) == 48, "scoped registration append");
_Static_assert(offsetof(MmsUiRefreshSubscriptionApiV1_1, queryScoped) == 56, "scoped query append");

/* mms.ui.refresh.subscribe 1.1 compile-time compatibility: natural alignment,
   scope and WAIT-bit values, unchanged global ABI minor, exact callback types. */
_Static_assert(_Alignof(MmsUiRefreshScopedSubscriptionDescV1) == 8, "scoped descriptor natural alignment");
_Static_assert(_Alignof(MmsUiRefreshScopedSubscriptionStateV1) == 8, "scoped state natural alignment");
_Static_assert(_Alignof(MmsUiRefreshSubscriptionApiV1_1) == 8, "scoped API natural alignment");
_Static_assert(offsetof(MmsUiRefreshScopedSubscriptionDescV1, h) == 0, "scoped descriptor header offset");
_Static_assert(MMS_UI_REFRESH_SELF == 1u && MMS_UI_REFRESH_SUBTREE == 2u && MMS_UI_REFRESH_DEPENDENTS == 4u, "refresh scope values");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH == 1u, "scoped wait graph bit");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET == 2u, "scoped wait target bit");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY == 4u, "scoped wait queue capacity bit");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE == 8u, "scoped wait source bit");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER == 16u, "scoped wait consumer bit");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_BINDING == 32u, "scoped wait binding bit");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE == 64u, "scoped wait host wake bit");
_Static_assert(MMS_UI_REFRESH_SUBSCRIPTION_WAIT_DISPLAY_CAPACITY == 128u, "scoped wait display capacity bit");
_Static_assert(MMS_ABI_MAJOR == 1u && MMS_ABI_MINOR == 0u, "service minor 1 keeps ABI minor 0");
_Static_assert(sizeof(MmsUiRefreshSubscriptionApiV1_1) == sizeof(MmsUiRefreshSubscriptionApiV1) + 2u * sizeof(void*), "service minor 1 only appends its own table");
_Static_assert(_Generic(((MmsUiRefreshSubscriptionApiV1_1*)0)->subscribeScoped, MmsResult (MMS_CALL *)(void*, const MmsUiRefreshScopedSubscriptionDescV1*, MmsHandle*): 1, default: 0) == 1, "scoped subscribe exact signature");
_Static_assert(_Generic(((MmsUiRefreshSubscriptionApiV1_1*)0)->queryScoped, MmsResult (MMS_CALL *)(void*, MmsHandle, MmsUiRefreshScopedSubscriptionStateV1*): 1, default: 0) == 1, "scoped query exact signature");
/* mms.ui.refresh.property 1.1 compile-time ABI: scoped capabilities, descriptor,
   state and API append-only layouts, 8-byte natural alignment, frozen WAIT bits
   and exact callback types. All 1.0 assertions above remain in force. */
_Static_assert(sizeof(MmsUiPropertyRefreshScopedCapabilitiesV1) == 40, "scoped property refresh capabilities");
_Static_assert(_Alignof(MmsUiPropertyRefreshScopedCapabilitiesV1) == 8, "scoped property refresh capabilities alignment");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedCapabilitiesV1, supportedScopes) == 32, "scoped property refresh capabilities scopes offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedCapabilitiesV1, reservedTail) == 36, "scoped property refresh capabilities tail offset");
#define MMS_PROPERTY_SCOPED_CAPS_PREFIX(field) _Static_assert(offsetof(MmsUiPropertyRefreshScopedCapabilitiesV1, field) == offsetof(MmsUiPropertyRefreshCapabilitiesV1, field), "scoped property refresh capabilities prefix " #field)
MMS_PROPERTY_SCOPED_CAPS_PREFIX(h); MMS_PROPERTY_SCOPED_CAPS_PREFIX(maxSubscriptionsPerPlugin);
MMS_PROPERTY_SCOPED_CAPS_PREFIX(supportedKinds); MMS_PROPERTY_SCOPED_CAPS_PREFIX(supportedFlags);
MMS_PROPERTY_SCOPED_CAPS_PREFIX(flags); MMS_PROPERTY_SCOPED_CAPS_PREFIX(reserved);
#undef MMS_PROPERTY_SCOPED_CAPS_PREFIX
_Static_assert(sizeof(MmsUiPropertyRefreshScopedSubscriptionDescV1) == 96, "scoped property refresh descriptor");
_Static_assert(_Alignof(MmsUiPropertyRefreshScopedSubscriptionDescV1) == 8, "scoped property refresh descriptor alignment");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, h) == 0, "scoped property refresh descriptor header offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, property) == 8, "scoped property refresh descriptor property offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, seed) == 40, "scoped property refresh descriptor seed offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, scope) == 72, "scoped property refresh descriptor scope offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, kinds) == 76, "scoped property refresh descriptor kinds offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, flags) == 80, "scoped property refresh descriptor flags offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, reserved) == 84, "scoped property refresh descriptor reserved offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionDescV1, reservedTail) == 88, "scoped property refresh descriptor tail offset");
_Static_assert(sizeof(MmsUiPropertyRefreshScopedSubscriptionStateV1) == 176, "scoped property refresh state");
_Static_assert(_Alignof(MmsUiPropertyRefreshScopedSubscriptionStateV1) == 8, "scoped property refresh state alignment");
#define MMS_PROPERTY_SCOPED_STATE_PREFIX(field) _Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, field) == offsetof(MmsUiPropertyRefreshSubscriptionStateV1, field), "scoped property refresh state prefix " #field)
MMS_PROPERTY_SCOPED_STATE_PREFIX(h); MMS_PROPERTY_SCOPED_STATE_PREFIX(subscription);
MMS_PROPERTY_SCOPED_STATE_PREFIX(target); MMS_PROPERTY_SCOPED_STATE_PREFIX(property);
MMS_PROPERTY_SCOPED_STATE_PREFIX(latestSequence); MMS_PROPERTY_SCOPED_STATE_PREFIX(submittedSequence);
MMS_PROPERTY_SCOPED_STATE_PREFIX(completedSequence); MMS_PROPERTY_SCOPED_STATE_PREFIX(lastResult);
MMS_PROPERTY_SCOPED_STATE_PREFIX(pending); MMS_PROPERTY_SCOPED_STATE_PREFIX(waitingFor);
MMS_PROPERTY_SCOPED_STATE_PREFIX(reserved);
#undef MMS_PROPERTY_SCOPED_STATE_PREFIX
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, scope) == 120, "scoped property refresh state scope offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, kinds) == 124, "scoped property refresh state kinds offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, flags) == 128, "scoped property refresh state flags offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, reservedTail0) == 132, "scoped property refresh state reserved tail0 offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, workGeneration) == 136, "scoped property refresh state work generation offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, submittedWorkGeneration) == 144, "scoped property refresh state submitted work generation offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, completedWorkGeneration) == 152, "scoped property refresh state completed work generation offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, submittedDependencyRevision) == 160, "scoped property refresh state dependency revision offset");
_Static_assert(offsetof(MmsUiPropertyRefreshScopedSubscriptionStateV1, reservedTail) == 168, "scoped property refresh state tail offset");
_Static_assert(sizeof(MmsUiPropertyRefreshApiV1_1) == 88, "scoped property refresh API");
_Static_assert(sizeof(MmsUiPropertyRefreshApiV1_1) == sizeof(MmsUiPropertyRefreshApiV1) + 3u * sizeof(void*), "scoped property refresh API appends exactly three slots");
_Static_assert(_Alignof(MmsUiPropertyRefreshApiV1_1) == 8, "scoped property refresh API alignment");
#define MMS_PROPERTY_SCOPED_API_PREFIX(field) _Static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, field) == offsetof(MmsUiPropertyRefreshApiV1, field), "scoped property refresh API prefix " #field)
MMS_PROPERTY_SCOPED_API_PREFIX(h); MMS_PROPERTY_SCOPED_API_PREFIX(context);
MMS_PROPERTY_SCOPED_API_PREFIX(getCapabilities); MMS_PROPERTY_SCOPED_API_PREFIX(getPropertyRef);
MMS_PROPERTY_SCOPED_API_PREFIX(subscribe); MMS_PROPERTY_SCOPED_API_PREFIX(notify);
MMS_PROPERTY_SCOPED_API_PREFIX(query); MMS_PROPERTY_SCOPED_API_PREFIX(unsubscribe);
#undef MMS_PROPERTY_SCOPED_API_PREFIX
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, getScopedCapabilities) == 64, "scoped property refresh API getScopedCapabilities offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, subscribeScoped) == 72, "scoped property refresh API subscribeScoped offset");
_Static_assert(offsetof(MmsUiPropertyRefreshApiV1_1, queryScoped) == 80, "scoped property refresh API queryScoped offset");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_BINDING == 1u, "property WAIT binding bit");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_TARGET == 2u, "property WAIT target bit");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY == 4u, "property WAIT capacity bit");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER == 8u, "property WAIT consumer bit");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE == 16u, "property WAIT host wake bit");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_GRAPH == 32u, "property WAIT graph bit");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE == 64u, "property WAIT source bit");
_Static_assert(MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY == 128u, "property WAIT display capacity bit");
_Static_assert(_Generic(((MmsUiPropertyRefreshApiV1_1*)0)->getScopedCapabilities, MmsResult (MMS_CALL *)(void*, MmsUiPropertyRefreshScopedCapabilitiesV1*): 1, default: 0) == 1, "scoped property getScopedCapabilities exact signature");
_Static_assert(_Generic(((MmsUiPropertyRefreshApiV1_1*)0)->subscribeScoped, MmsResult (MMS_CALL *)(void*, const MmsUiPropertyRefreshScopedSubscriptionDescV1*, MmsHandle*): 1, default: 0) == 1, "scoped property subscribeScoped exact signature");
_Static_assert(_Generic(((MmsUiPropertyRefreshApiV1_1*)0)->queryScoped, MmsResult (MMS_CALL *)(void*, MmsHandle, MmsUiPropertyRefreshScopedSubscriptionStateV1*): 1, default: 0) == 1, "scoped property queryScoped exact signature");

#include <mms/evaluation.h>
_Static_assert(sizeof(MmsEvaluationCapsV1) == 40, "MmsEvaluationCapsV1 layout");
_Static_assert(sizeof(MmsEvaluationSourceQueryV1) == 48, "MmsEvaluationSourceQueryV1 layout");
_Static_assert(sizeof(MmsEvaluationSourceInfoV1) == 64, "MmsEvaluationSourceInfoV1 layout");
_Static_assert(sizeof(MmsEvaluationBoneV1) == 32, "MmsEvaluationBoneV1 layout");
_Static_assert(sizeof(MmsBoneChannelsV1) == 80, "MmsBoneChannelsV1 layout");
_Static_assert(sizeof(MmsEvaluationApiV1) == 72, "MmsEvaluationApiV1 layout");
_Static_assert(offsetof(MmsEvaluationApiV1, context) == 8, "evaluation context offset");
_Static_assert(offsetof(MmsEvaluationApiV1, getCapabilities) == 16, "evaluation getCapabilities offset");
_Static_assert(offsetof(MmsEvaluationApiV1, createSource) == 24, "evaluation createSource offset");
_Static_assert(offsetof(MmsEvaluationApiV1, describeSource) == 32, "evaluation describeSource offset");
_Static_assert(offsetof(MmsEvaluationApiV1, boneAt) == 40, "evaluation boneAt offset");
_Static_assert(offsetof(MmsEvaluationApiV1, sampleBone) == 48, "evaluation sampleBone offset");
_Static_assert(offsetof(MmsEvaluationApiV1, retain) == 56, "evaluation retain offset");
_Static_assert(offsetof(MmsEvaluationApiV1, release) == 64, "evaluation release offset");
_Static_assert(_Generic(((MmsEvaluationApiV1*)0)->sampleBone, MmsResult (MMS_CALL *)(void*, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*):1, default:0), "evaluation sample signature");

_Static_assert(sizeof(MmsEvaluationSourceInfoV1_1) == 72, "MmsEvaluationSourceInfoV1_1 layout");
_Static_assert(sizeof(MmsEvaluationMorphV1) == 32, "MmsEvaluationMorphV1 layout");
_Static_assert(sizeof(MmsMorphWeightV1) == 16, "MmsMorphWeightV1 layout");
_Static_assert(sizeof(MmsEvaluationApiV1_1) == 104, "MmsEvaluationApiV1_1 layout");
_Static_assert(offsetof(MmsEvaluationApiV1_1, context) == 8, "evaluation1.1 context offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, getCapabilities) == 16, "evaluation1.1 getCapabilities offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, createSource) == 24, "evaluation1.1 createSource offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, describeSource) == 32, "evaluation1.1 describeSource offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, boneAt) == 40, "evaluation1.1 boneAt offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, sampleBone) == 48, "evaluation1.1 sampleBone offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, retain) == 56, "evaluation1.1 retain offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, release) == 64, "evaluation1.1 release offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, createSourceChannels) == 72, "evaluation1.1 createSourceChannels offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, describeSourceChannels) == 80, "evaluation1.1 describeSourceChannels offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, morphAt) == 88, "evaluation1.1 morphAt offset");
_Static_assert(offsetof(MmsEvaluationApiV1_1, sampleMorph) == 96, "evaluation1.1 sampleMorph offset");
_Static_assert(offsetof(MmsEvaluationSourceInfoV1_1, morphCount) == 64, "extended source prefix");

_Static_assert(sizeof(MmsEvaluationSourceInfoV1_2) == 80, "MmsEvaluationSourceInfoV1_2 layout");
_Static_assert(sizeof(MmsEvaluationIkV1) == 24, "MmsEvaluationIkV1 layout");
_Static_assert(sizeof(MmsEvaluationSelectorV1) == 32, "MmsEvaluationSelectorV1 layout");
_Static_assert(sizeof(MmsDisplayStateV1) == 16, "MmsDisplayStateV1 layout");
_Static_assert(sizeof(MmsIkStateV1) == 16, "MmsIkStateV1 layout");
_Static_assert(sizeof(MmsParentSelectionV1) == 64, "MmsParentSelectionV1 layout");
_Static_assert(sizeof(MmsEvaluationApiV1_2) == 152, "MmsEvaluationApiV1_2 layout");
_Static_assert(offsetof(MmsEvaluationApiV1_2, context) == 8, "evaluation1.2 context offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, getCapabilities) == 16, "evaluation1.2 getCapabilities offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, createSource) == 24, "evaluation1.2 createSource offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, describeSource) == 32, "evaluation1.2 describeSource offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, boneAt) == 40, "evaluation1.2 boneAt offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, sampleBone) == 48, "evaluation1.2 sampleBone offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, retain) == 56, "evaluation1.2 retain offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, release) == 64, "evaluation1.2 release offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, createSourceChannels) == 72, "evaluation1.2 createSourceChannels offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, describeSourceChannels) == 80, "evaluation1.2 describeSourceChannels offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, morphAt) == 88, "evaluation1.2 morphAt offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, sampleMorph) == 96, "evaluation1.2 sampleMorph offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, describeSourceDisplay) == 104, "evaluation1.2 describeSourceDisplay offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, ikAt) == 112, "evaluation1.2 ikAt offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, selectorAt) == 120, "evaluation1.2 selectorAt offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, sampleVisibility) == 128, "evaluation1.2 sampleVisibility offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, sampleIk) == 136, "evaluation1.2 sampleIk offset");
_Static_assert(offsetof(MmsEvaluationApiV1_2, sampleParent) == 144, "evaluation1.2 sampleParent offset");
_Static_assert(offsetof(MmsEvaluationSourceInfoV1_2, ikCount) == 72, "evaluation1.2 source prefix");
_Static_assert(_Generic(((MmsEvaluationApiV1_2*)0)->sampleParent, MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*):1, default:0), "parent sample signature");

_Static_assert(sizeof(MmsParentStateV1) == 64, "parent state layout");
_Static_assert(sizeof(MmsEvaluationApiV1_3) == 160, "evaluation1.3 layout");
_Static_assert(offsetof(MmsEvaluationApiV1_3, sampleParentState) == sizeof(MmsEvaluationApiV1_2), "evaluation1.3 preserves prefix");
_Static_assert(offsetof(MmsParentStateV1, h) == 0, "parent state h offset");
_Static_assert(offsetof(MmsParentStateV1, target) == 8, "parent state target offset");
_Static_assert(offsetof(MmsParentStateV1, bone) == 32, "parent state bone offset");
_Static_assert(offsetof(MmsParentStateV1, windowStart) == 48, "parent state windowStart offset");
_Static_assert(offsetof(MmsParentStateV1, windowEnd) == 52, "parent state windowEnd offset");
_Static_assert(offsetof(MmsParentStateV1, kind) == 56, "parent state kind offset");
_Static_assert(offsetof(MmsParentStateV1, reserved) == 60, "parent state reserved offset");
_Static_assert(_Generic(((MmsEvaluationApiV1_3*)0)->sampleParentState, MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*):1, default:0), "parent state sample signature");

_Static_assert(sizeof(MmsEvaluationSceneCapsV1) == 72, "MmsEvaluationSceneCapsV1 layout");
_Static_assert(sizeof(MmsEvaluationSceneQueryV1) == 24, "MmsEvaluationSceneQueryV1 layout");
_Static_assert(sizeof(MmsEvaluationSceneInfoV1) == 40, "MmsEvaluationSceneInfoV1 layout");
_Static_assert(sizeof(MmsEvaluationSceneModelV1) == 48, "MmsEvaluationSceneModelV1 layout");
_Static_assert(sizeof(MmsEvaluationSceneBoneV1) == 32, "MmsEvaluationSceneBoneV1 layout");
_Static_assert(sizeof(MmsEvaluationFrameQueryV1) == 56, "MmsEvaluationFrameQueryV1 layout");
_Static_assert(sizeof(MmsEvaluationFrameInfoV1) == 64, "MmsEvaluationFrameInfoV1 layout");
_Static_assert(sizeof(MmsEvaluationScenePoseV1) == 296, "MmsEvaluationScenePoseV1 layout");
_Static_assert(sizeof(MmsEvaluationApiV1_4) == 232, "MmsEvaluationApiV1_4 layout");
_Static_assert(offsetof(MmsEvaluationApiV1_4, context) == offsetof(MmsEvaluationApiV1_3, context), "evaluation1.4 context prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, getCapabilities) == offsetof(MmsEvaluationApiV1_3, getCapabilities), "evaluation1.4 getCapabilities prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, createSource) == offsetof(MmsEvaluationApiV1_3, createSource), "evaluation1.4 createSource prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, describeSource) == offsetof(MmsEvaluationApiV1_3, describeSource), "evaluation1.4 describeSource prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, boneAt) == offsetof(MmsEvaluationApiV1_3, boneAt), "evaluation1.4 boneAt prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, sampleBone) == offsetof(MmsEvaluationApiV1_3, sampleBone), "evaluation1.4 sampleBone prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, retain) == offsetof(MmsEvaluationApiV1_3, retain), "evaluation1.4 retain prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, release) == offsetof(MmsEvaluationApiV1_3, release), "evaluation1.4 release prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, createSourceChannels) == offsetof(MmsEvaluationApiV1_3, createSourceChannels), "evaluation1.4 createSourceChannels prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, describeSourceChannels) == offsetof(MmsEvaluationApiV1_3, describeSourceChannels), "evaluation1.4 describeSourceChannels prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, morphAt) == offsetof(MmsEvaluationApiV1_3, morphAt), "evaluation1.4 morphAt prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, sampleMorph) == offsetof(MmsEvaluationApiV1_3, sampleMorph), "evaluation1.4 sampleMorph prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, describeSourceDisplay) == offsetof(MmsEvaluationApiV1_3, describeSourceDisplay), "evaluation1.4 describeSourceDisplay prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, ikAt) == offsetof(MmsEvaluationApiV1_3, ikAt), "evaluation1.4 ikAt prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, selectorAt) == offsetof(MmsEvaluationApiV1_3, selectorAt), "evaluation1.4 selectorAt prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, sampleVisibility) == offsetof(MmsEvaluationApiV1_3, sampleVisibility), "evaluation1.4 sampleVisibility prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, sampleIk) == offsetof(MmsEvaluationApiV1_3, sampleIk), "evaluation1.4 sampleIk prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, sampleParent) == offsetof(MmsEvaluationApiV1_3, sampleParent), "evaluation1.4 sampleParent prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, sampleParentState) == offsetof(MmsEvaluationApiV1_3, sampleParentState), "evaluation1.4 sampleParentState prefix");
_Static_assert(offsetof(MmsEvaluationApiV1_4, getSceneCapabilities) == sizeof(MmsEvaluationApiV1_3), "evaluation1.4 extension offset");
_Static_assert(_Generic(((MmsEvaluationApiV1_4*)0)->readBonePose, MmsResult (MMS_CALL *)(void*, MmsHandle, uint32_t, uint32_t, MmsEvaluationScenePoseV1*):1, default:0), "scene pose signature");
