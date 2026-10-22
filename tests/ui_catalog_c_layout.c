/* Compile as C, independently of the C++ SDK. The nested packing must restore
   an including C plugin's prior state rather than leak the ABI's pack(8). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#pragma pack(push, 1)
#include <mms/ui_catalog.h>
struct CallerPackingAfterCatalog { char first; uint64_t second; };
_Static_assert(offsetof(struct CallerPackingAfterCatalog, second) == 1, "catalog restores caller packing");
_Static_assert(sizeof(struct CallerPackingAfterCatalog) == 9, "caller pack(1) survives header");
#pragma pack(pop)
_Static_assert(sizeof(void*) == 8, "catalog C ABI is x64");
_Static_assert(sizeof(MmsUiCatalogEntryV1) == 200, "entry size");
_Static_assert(offsetof(MmsUiCatalogEntryV1, element) == 8, "entry original element");
_Static_assert(offsetof(MmsUiCatalogEntryV1, id) == 16, "entry borrowed identifier");
_Static_assert(offsetof(MmsUiCatalogEntryV1, descriptionGeneration) == 112, "entry description revision");
_Static_assert(offsetof(MmsUiCatalogEntryV1, target) == 120, "entry original target");
_Static_assert(offsetof(MmsUiCatalogEntryV1, bounds) == 152, "entry DIP bounds");
_Static_assert(offsetof(MmsUiCatalogEntryV1, kind) == 168, "entry kind");
_Static_assert(offsetof(MmsUiCatalogEntryV1, knownFields) == 180, "entry known fields");
_Static_assert(offsetof(MmsUiCatalogEntryV1, reserved) == 196, "entry tail");
_Static_assert(sizeof(MmsUiCatalogRegistrationV1) == 72, "registration size");
_Static_assert(offsetof(MmsUiCatalogRegistrationV1, handle) == 8, "registration original handle");
_Static_assert(offsetof(MmsUiCatalogRegistrationV1, kind) == 64, "registration kind");
_Static_assert(sizeof(MmsUiCatalogCapabilitiesV1) == 56, "capabilities size");
_Static_assert(offsetof(MmsUiCatalogCapabilitiesV1, maxLeasesGlobal) == 16, "global lease quota");
_Static_assert(offsetof(MmsUiCatalogCapabilitiesV1, maxRetainedBytesGlobal) == 40, "global memory quota");
_Static_assert(sizeof(MmsUiCatalogSnapshotV1) == 104, "snapshot size");
_Static_assert(offsetof(MmsUiCatalogSnapshotV1, projectEpoch) == 32, "snapshot original project epoch");
_Static_assert(offsetof(MmsUiCatalogSnapshotV1, elements) == 56, "snapshot elements span");
_Static_assert(offsetof(MmsUiCatalogSnapshotV1, registrations) == 80, "snapshot registrations span");
_Static_assert(sizeof(MmsUiCatalogApiV1) == 48, "table size");
_Static_assert(offsetof(MmsUiCatalogApiV1, context) == 8, "table borrowed context");
_Static_assert(offsetof(MmsUiCatalogApiV1, getCapabilities) == 16, "capabilities slot");
_Static_assert(offsetof(MmsUiCatalogApiV1, acquireSnapshot) == 24, "acquire slot");
_Static_assert(offsetof(MmsUiCatalogApiV1, resolveElement) == 32, "resolve slot");
_Static_assert(offsetof(MmsUiCatalogApiV1, releaseSnapshot) == 40, "release slot");
int main(void) {
    puts("PASS UI catalog standalone C layout and packing contracts");
    return 0;
}
