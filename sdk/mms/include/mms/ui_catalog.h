#ifndef MMS_UI_CATALOG_H
#define MMS_UI_CATALOG_H
#include "ui.h"
#include "refresh.h"

#define MMS_UI_CATALOG_REGISTRY 1u
#define MMS_UI_CATALOG_MAIN_MENU 2u
#define MMS_UI_CATALOG_MAIN_CONTROLS 4u
#define MMS_UI_CATALOG_REGISTERED 1u
#define MMS_UI_CATALOG_HOST_ANCHOR 2u
#define MMS_UI_CATALOG_NATIVE_OBSERVATION 3u
#define MMS_UI_CATALOG_LABEL 1u
#define MMS_UI_CATALOG_ENABLED 2u
#define MMS_UI_CATALOG_VISIBLE 4u
#define MMS_UI_CATALOG_CHECKED 8u
#define MMS_UI_CATALOG_BOUNDS 16u
#define MMS_UI_CATALOG_PRESENTATION 32u
#define MMS_UI_CATALOG_STATE_ENABLED 1u
#define MMS_UI_CATALOG_STATE_VISIBLE 2u
#define MMS_UI_CATALOG_STATE_CHECKED 4u
#define MMS_UI_CATALOG_COMMAND 1u
#define MMS_UI_CATALOG_PROPERTY 2u
#define MMS_UI_CATALOG_REGISTRY_COMPLETE 1u
#define MMS_UI_CATALOG_MATERIALIZED_MENU 2u
#define MMS_UI_CATALOG_DECLARED_CONTROLS 4u
#define MMS_UI_CATALOG_INCOMPLETE_NATIVE 8u
#define MMS_UI_CATALOG_ROLE_UNKNOWN 0u
#define MMS_UI_CATALOG_ROLE_BUTTON 1u
#define MMS_UI_CATALOG_ROLE_INPUT 2u
#define MMS_UI_CATALOG_ROLE_COMBO 3u
#define MMS_UI_CATALOG_ROLE_SLIDER 4u
#define MMS_UI_CATALOG_ROLE_LABEL 5u
#define MMS_UI_CATALOG_ROLE_SCROLLBAR 6u
#define MMS_UI_CATALOG_ROLE_TABS 7u
#define MMS_UI_CATALOG_ROLE_MENU 8u
#define MMS_UI_CATALOG_ROLE_MENU_ITEM 9u
#define MMS_UI_CATALOG_ROLE_SEPARATOR 10u

#pragma pack(push, 8)
/* mms.ui.catalog 1.0. UI-thread calls only; acquired memory is immutable
   and can be read on workers until release on the UI thread. No call reads
   native windows, invokes providers, refreshes, changes focus or edits data.
   A catalog target uses the ORIGINAL element handle. Read access grants no
   command, layout, refresh or cross-owner write permission. */
typedef struct MmsUiCatalogEntryV1 {
	MmsHeader h;
	MmsHandle element;
	MmsString id, ownerPlugin, parentId, label, commandId, propertyId;
	uint64_t descriptionGeneration;
	MmsUiTargetRefV1 target;
	MmsUiDirtyRectV1 bounds;
	uint32_t kind, origin, scope, knownFields, state, role, ordinal, reserved;
} MmsUiCatalogEntryV1;

/* Registration descriptions only, never callback or opaque DLL pointers.
   Read full argument/property schemas through the original mms.ui snapshot. */
typedef struct MmsUiCatalogRegistrationV1 {
	MmsHeader h;
	MmsHandle handle;
	MmsString id, ownerPlugin, label;
	uint32_t kind, flags;
} MmsUiCatalogRegistrationV1;

typedef struct MmsUiCatalogCapabilitiesV1 {
	MmsHeader h;
	uint32_t supportedScopes, maxLeasesPerPlugin;
	uint64_t maxLeasesGlobal, maxSnapshotBytes, maxRetainedBytesPerPlugin, maxRetainedBytesGlobal;
	uint32_t maxEntries, reserved;
} MmsUiCatalogCapabilitiesV1;

/* revision covers native observation; registryRevision covers descriptions.
   Compare both, but neither is a change token for cached plugin geometry or
   presentation. Resolve the exact target before using its current metadata.
   Epoch is the original refresh project epoch. availableScopes describes the last
   successful host publication, not all UI the application could ever create.
   knownFields distinguishes unknown labels/states/geometry. Bounds are DIPs
   in the main-window client space for native controls; registered plugin
   bounds use the refresh target's published coordinate space. Menu rows have
   no BOUNDS. Native state and registered labels are last-published metadata,
   not an instruction to evaluate command state or read an input's value.
   An unmaterialized element has instanceGeneration=0 and no PRESENTATION.
   Snapshots never silently truncate. scopes must be a nonzero supported mask. */
typedef struct MmsUiCatalogSnapshotV1 {
	MmsHeader h;
	MmsHandle handle;
	uint64_t revision, registryRevision, projectEpoch;
	uint32_t requestedScopes, availableScopes, coverageFlags, reserved;
	MmsSpan elements, registrations;
} MmsUiCatalogSnapshotV1;

typedef struct MmsUiCatalogApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *getCapabilities)(void*, MmsUiCatalogCapabilitiesV1*);
	MmsResult (MMS_CALL *acquireSnapshot)(void*, uint32_t scopes, MmsUiCatalogSnapshotV1*);
	/* expected must match the current handle, presentation and project epoch.
	   Success returns one leased entry; all failure outputs remain unchanged. */
	MmsResult (MMS_CALL *resolveElement)(void*, const MmsUiTargetRefV1* expected,
		MmsUiCatalogEntryV1*, MmsHandle* lease);
	/* Release remains available during stop, with exact owner/thread checks. */
	MmsResult (MMS_CALL *releaseSnapshot)(void*, MmsHandle);
} MmsUiCatalogApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsUiCatalogEntryV1) == 200, "MMS UI catalog entry");
static_assert(sizeof(MmsUiCatalogRegistrationV1) == 72, "MMS UI catalog registration");
static_assert(sizeof(MmsUiCatalogCapabilitiesV1) == 56, "MMS UI catalog capabilities");
static_assert(sizeof(MmsUiCatalogSnapshotV1) == 104, "MMS UI catalog snapshot");
static_assert(sizeof(MmsUiCatalogApiV1) == 48, "MMS UI catalog API");
#endif
#endif
