#ifndef MMS_PROPOSAL_PLUGIN_H
#define MMS_PROPOSAL_PLUGIN_H
/* MMS C ABI 1.0. Query each service before use; availability is host-specific. */
#include <stdint.h>
#include <stddef.h>
#if !defined(_WIN32) || !defined(_WIN64)
#error MMS V1 targets Windows x64.
#endif
#define MMS_CALL __cdecl
#define MMS_ABI_MAJOR 1u
#define MMS_ABI_MINOR 0u
#define MMS_OK 0
#define MMS_E_ARGUMENT (-1)
#define MMS_E_VERSION (-2)
#define MMS_E_UNSUPPORTED (-3)
#define MMS_E_NOT_FOUND (-4)
#define MMS_E_STALE (-5)
#define MMS_E_THREAD (-6)
#define MMS_E_BUSY (-7)
#define MMS_E_NOT_READY (-8)
#define MMS_E_CONFLICT (-9)
#define MMS_E_MEMORY (-10)
#define MMS_E_FAILED (-11)
#define MMS_E_CANCELLED (-12)
#define MMS_COMPUTE_AUTO 0u
#define MMS_COMPUTE_CPU 1u
#define MMS_COMPUTE_GPU 2u
#pragma pack(push, 8)
typedef int32_t MmsResult;
typedef uint64_t MmsHandle;
typedef struct MmsId { uint64_t lo, hi; } MmsId;
typedef struct MmsHeader { uint32_t size; uint16_t major, minor; } MmsHeader;
typedef struct MmsString { const char* data; uint64_t bytes; } MmsString;
typedef struct MmsBytes { const void* data; uint64_t bytes; } MmsBytes;
typedef struct MmsSpan {
	const void* data;
	uint64_t count;
	uint32_t stride, recordVersion;
} MmsSpan;
typedef struct MmsTime { int64_t numerator; uint64_t denominator; } MmsTime;
typedef struct MmsObjectRef { MmsId id; uint64_t generation; } MmsObjectRef;
#define MMS_VALUE_EMPTY 0u
#define MMS_VALUE_BOOL 1u
#define MMS_VALUE_INT 2u
#define MMS_VALUE_UINT 3u
#define MMS_VALUE_NUMBER 4u
#define MMS_VALUE_VECTOR 5u
#define MMS_VALUE_TEXT 6u
#define MMS_VALUE_ID 7u
#define MMS_VALUE_BYTES 8u
typedef struct MmsValue {
	uint32_t type, components;
	union {
		int64_t signedValue;
		uint64_t unsignedValue;
		double number;
		double vector[4];
		MmsString text;
		MmsId id;
		MmsBytes bytes;
	} value;
} MmsValue;
typedef struct MmsNamedValue { MmsString name; MmsValue value; } MmsNamedValue;
typedef struct MmsCreateInfoV1 {
	MmsHeader h;
	MmsString pluginId, packagePath, userDataPath;
	uint32_t computePolicy, reserved;
} MmsCreateInfoV1;
typedef struct MmsEventV1 {
	MmsHeader h;
	uint32_t kind, payloadVersion;
	uint64_t sequence;
	MmsObjectRef object;
	MmsBytes payload;
} MmsEventV1;
#define MMS_EVENT_PROJECT_OPENED 1u
#define MMS_EVENT_PROJECT_CLOSED 2u
#define MMS_EVENT_PROJECT_SAVED 3u
#define MMS_EVENT_SCENE_RESET 4u
#define MMS_EVENT_OBJECT_REMOVED 5u
#define MMS_EVENT_DEVICE_LOST 6u
#define MMS_EVENT_DEVICE_RESTORED 7u
#define MMS_EVENT_LANGUAGE_CHANGED 8u
#define MMS_EVENT_UI_REGISTRY_CHANGED 9u
#define MMS_EVENT_PROPERTY_CHANGED 10u
#define MMS_EVENT_LAYOUT_CHANGED 11u
#define MMS_EVENT_COMPUTE_POLICY_CHANGED 12u
#define MMS_EVENT_UI_READY 13u
#define MMS_EVENT_UI_CLOSING 14u
#define MMS_EVENT_COMMAND_STATE_CHANGED 15u
#define MMS_EVENT_DPI_CHANGED 16u
typedef struct MmsRevisionEventV1 { MmsHeader h; uint64_t revision; } MmsRevisionEventV1;
typedef struct MmsProjectEventV1 {
	MmsHeader h;
	MmsId projectId;
	uint64_t revision;
	MmsString path;
} MmsProjectEventV1;
typedef struct MmsPropertyEventV1 {
	MmsHeader h;
	MmsString propertyId;
	MmsObjectRef target;
	uint64_t revision;
} MmsPropertyEventV1;
typedef struct MmsDeviceEventV1 {
	MmsHeader h;
	uint64_t deviceGeneration;
	uint32_t backend, reserved;
} MmsDeviceEventV1;
typedef struct MmsCodeEventV1 { MmsHeader h; uint32_t value, reserved; } MmsCodeEventV1;
typedef struct MmsHostV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *queryService)(void*, MmsString, uint32_t, uint32_t, uint32_t, void*);
	MmsResult (MMS_CALL *registerProvider)(void*, MmsString, MmsString, const void*, uint32_t, MmsHandle*);
	MmsResult (MMS_CALL *unregisterProvider)(void*, MmsHandle);
	void (MMS_CALL *log)(void*, uint32_t, MmsString);
	MmsResult (MMS_CALL *enqueueUi)(void*, void (MMS_CALL *)(void*), void (MMS_CALL *)(void*), void*, MmsHandle*);
	MmsResult (MMS_CALL *cancelUi)(void*, MmsHandle);
} MmsHostV1;
typedef struct MmsPluginApiV1 {
	MmsHeader h;
	MmsResult (MMS_CALL *create)(const MmsHostV1*, const MmsCreateInfoV1*, void**);
	MmsResult (MMS_CALL *start)(void*);
	void (MMS_CALL *onEvent)(void*, const MmsEventV1*);
	MmsResult (MMS_CALL *stop)(void*);
	void (MMS_CALL *destroy)(void*);
} MmsPluginApiV1;
typedef MmsResult (MMS_CALL *MmsPluginGetApiFn)(uint32_t, uint32_t, uint32_t, MmsPluginApiV1*);
#pragma pack(pop)
#ifdef __cplusplus
extern "C" {
#endif
MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minMinor, uint32_t outBytes, MmsPluginApiV1* outApi);
#ifdef __cplusplus
}
static_assert(sizeof(MmsHeader) == 8 && sizeof(MmsSpan) == 24, "MMS packing");
static_assert(sizeof(MmsValue) == 40 && offsetof(MmsValue, value) == 8, "MMS value layout");
static_assert(sizeof(MmsHostV1) == 64 && sizeof(MmsPluginApiV1) == 48, "MMS API layout");
#endif
#endif
