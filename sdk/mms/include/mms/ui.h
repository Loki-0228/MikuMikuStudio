#ifndef MMS_PROPOSAL_UI_H
#define MMS_PROPOSAL_UI_H
#include "plugin.h"
#define MMS_COMMAND_EDIT 1u
#define MMS_COMMAND_OBJECT 2u
#define MMS_COMMAND_INVOKE_ONLY 4u
#define MMS_PROPERTY_READ_ONLY 1u
#define MMS_PROPERTY_UNDOABLE 2u
#define MMS_PROPERTY_ANIMATABLE 4u
#define MMS_PROPERTY_OBJECT 8u
#define MMS_ARGUMENT_REQUIRED 1u
#define MMS_UI_MENU 1u
#define MMS_UI_MENU_ITEM 2u
#define MMS_UI_BUTTON 3u
#define MMS_UI_INPUT 4u
#define MMS_UI_TOOLBAR 5u
#define MMS_UI_PANEL 6u
#define MMS_UI_SEPARATOR 7u
#define MMS_UI_GROUP 8u
#define MMS_UI_MOVE 1u
#define MMS_UI_HIDE 2u
#define MMS_UI_RESIZE 4u
#define MMS_UI_DOCK 8u
#define MMS_UI_SHORTCUT 16u
#define MMS_UI_CONTAINER 32u
#define MMS_LAYOUT_MOVE 1u
#define MMS_LAYOUT_VISIBLE 2u
#define MMS_LAYOUT_CREATE_GROUP 3u
#define MMS_LAYOUT_SIZE 4u
#define MMS_LAYOUT_DOCK 5u
#define MMS_LAYOUT_SHORTCUT 6u
#define MMS_LAYOUT_RESTORE 7u
#define MMS_LAYOUT_REMOVE_GROUP 8u
#pragma pack(push, 8)
typedef struct MmsChoiceV1 { MmsHeader h; MmsString label; MmsValue value; } MmsChoiceV1;
typedef struct MmsArgumentV1 {
	MmsHeader h;
	MmsString name;
	uint32_t valueType, components, flags, reserved;
	MmsValue defaultValue, minimum, maximum;
	MmsSpan choices;
} MmsArgumentV1;
typedef struct MmsCommandInfoV1 {
	MmsHeader h;
	MmsString id, ownerPlugin, label, description;
	MmsSpan arguments;
	uint32_t flags, reserved;
} MmsCommandInfoV1;
typedef struct MmsCommandStateV1 {
	MmsHeader h;
	uint32_t enabled, checked, visible, reserved;
	MmsString disabledReason;
} MmsCommandStateV1;
typedef struct MmsCommandDescV1 {
	MmsCommandInfoV1 info;
	void* context;
	MmsResult (MMS_CALL *getState)(void*, MmsObjectRef, MmsCommandStateV1*);
	MmsResult (MMS_CALL *invoke)(void*, MmsObjectRef, MmsSpan, MmsHandle);
} MmsCommandDescV1;
typedef struct MmsPropertyInfoV1 {
	MmsHeader h;
	MmsString id, ownerPlugin, label, unit;
	uint32_t valueType, components, flags, reserved;
	MmsValue defaultValue, minimum, maximum;
	MmsSpan choices;
} MmsPropertyInfoV1;
typedef struct MmsPropertyDescV1 {
	MmsPropertyInfoV1 info;
	void* context;
	MmsResult (MMS_CALL *read)(void*, MmsObjectRef, MmsValue*, MmsHandle*);
	void (MMS_CALL *releaseValue)(void*, MmsHandle);
	MmsResult (MMS_CALL *stage)(void*, MmsHandle, MmsObjectRef, const MmsValue*);
} MmsPropertyDescV1;
typedef struct MmsUiElementV1 {
	MmsHeader h;
	MmsString id, ownerPlugin, parentId, label, commandId, propertyId, surfaceId;
	uint32_t kind, capabilities;
	uint64_t generation;
} MmsUiElementV1;
typedef struct MmsUiSnapshotV1 {
	MmsHeader h;
	MmsHandle handle;
	uint64_t revision;
	MmsSpan commands, elements, properties;
} MmsUiSnapshotV1;
typedef struct MmsLayoutOpV1 {
	MmsHeader h;
	uint32_t operation, flags;
	MmsString elementId, parentId, beforeId;
	uint64_t expectedGeneration;
	union {
		uint32_t visible;
		MmsString groupLabel;
		struct { float width, height; } size;
		struct { uint32_t edge, order; float weight; } dock;
		struct { uint32_t virtualKey, modifiers, scope, reserved; } shortcut;
	} value;
} MmsLayoutOpV1;
typedef struct MmsUiApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *registerCommand)(void*, const MmsCommandDescV1*, MmsHandle*);
	MmsResult (MMS_CALL *registerElement)(void*, const MmsUiElementV1*, MmsHandle*);
	MmsResult (MMS_CALL *registerProperty)(void*, const MmsPropertyDescV1*, MmsHandle*);
	MmsResult (MMS_CALL *updateCommand)(void*, MmsHandle, const MmsCommandDescV1*);
	MmsResult (MMS_CALL *updateElement)(void*, MmsHandle, const MmsUiElementV1*);
	MmsResult (MMS_CALL *updateProperty)(void*, MmsHandle, const MmsPropertyDescV1*);
	MmsResult (MMS_CALL *unregisterItem)(void*, MmsHandle);
	MmsResult (MMS_CALL *acquireSnapshot)(void*, uint32_t, MmsUiSnapshotV1*);
	void (MMS_CALL *releaseSnapshot)(void*, MmsHandle);
	MmsResult (MMS_CALL *getCommandState)(void*, MmsString, MmsObjectRef, MmsCommandStateV1*, MmsHandle*);
	MmsResult (MMS_CALL *invoke)(void*, MmsString, MmsObjectRef, MmsSpan);
	MmsResult (MMS_CALL *readProperty)(void*, MmsString, MmsObjectRef, MmsValue*, MmsHandle*);
	void (MMS_CALL *releaseValue)(void*, MmsHandle);
	MmsResult (MMS_CALL *stageProperty)(void*, MmsHandle, MmsString, MmsObjectRef, const MmsValue*);
	MmsResult (MMS_CALL *beginLayout)(void*, uint64_t, MmsHandle*);
	MmsResult (MMS_CALL *applyLayout)(void*, MmsHandle, const MmsLayoutOpV1*);
	MmsResult (MMS_CALL *commitLayout)(void*, MmsHandle);
	void (MMS_CALL *cancelLayout)(void*, MmsHandle);
} MmsUiApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiApiV1) == 160, "MMS UI API layout");
static_assert(sizeof(MmsCommandInfoV1) == 104, "MMS command info layout");
static_assert(sizeof(MmsCommandDescV1) == 128, "MMS command registration layout");
static_assert(sizeof(MmsUiElementV1) == 136, "MMS element layout");
static_assert(sizeof(MmsLayoutOpV1) == 88, "MMS layout operation layout");
#endif
#endif
