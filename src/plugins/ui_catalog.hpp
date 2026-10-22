#pragma once
#include <mms/ui_catalog.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mikumikustudio::plugins {
class UiRegistry;
struct UiClient;
// Trusted adapter-owned metadata. IDs of new observations use mms.catalog.*.
// reuseElement aliases an existing non-observation element; it never creates
// another handle or changes that element's registration description.
struct HostUiCatalogEntry {
    std::string id, parentId, label;
    MmsHandle reuseElement = 0;
    std::uint64_t instance = 0;
    MmsUiDirtyRectV1 bounds{};
    std::uint32_t kind = MMS_UI_GROUP, role = MMS_UI_CATALOG_ROLE_UNKNOWN;
    std::uint32_t scope = 0, knownFields = 0, state = 0, ordinal = 0;
};
struct HostUiCatalogPublication {
    std::vector<HostUiCatalogEntry> entries;
    std::uint32_t availableScopes = 0, coverageFlags = MMS_UI_CATALOG_INCOMPLETE_NATIVE;
};
class UiCatalog {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit UiCatalog(UiRegistry&);
    ~UiCatalog();
    MmsUiCatalogApiV1 Api(UiClient&);
    // No Win32 queries/providers/Changed/SyncTopology. Validate and prepare
    // whole registry+metadata candidates, then swap in one no-fail publication.
    MmsResult Publish(const HostUiCatalogPublication&) noexcept;
    // Called before native instance destruction. No allocation or callbacks.
    void RetireInstance(std::uint64_t) noexcept;
    void Suspend() noexcept;
    bool HasLeases(const UiClient&) const noexcept;
};
}
