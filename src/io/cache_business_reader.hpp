#pragma once
#include "cache_business_codec.hpp"
#include "cache_source_registry.hpp"
#include "mikumikustudio/cache_catalog.hpp"
#include <memory>

namespace mikumikustudio::cache_business {
enum class ReadResult {
    Ok, Invalid, RecoveryRequired, Busy, Io, Unsupported, Limit, AllocationFailure
};

// An immutable view of one supplied catalog snapshot, with leases on all of
// its actual index pages. This is not a claim that the snapshot is the latest
// head, and does not grant cache budget, publication or deletion permission.
// It does not bind a business-store root or hold a lease on the catalog manifest
// file. Page leases and the copied catalog CAS token are its only retention.
class PublishedIndex {
public:
    PublishedIndex() noexcept;
    ~PublishedIndex();
    PublishedIndex(PublishedIndex&&) noexcept;
    PublishedIndex& operator=(PublishedIndex&&) noexcept;
    PublishedIndex(const PublishedIndex&) = delete;
    PublishedIndex& operator=(const PublishedIndex&) = delete;

    bool IsValid() const noexcept;
    // The snapshot copy retains its existing catalog CAS identity. It does not
    // acquire an additional catalog handle or make the source current.
    const cache_catalog::Snapshot& Source() const noexcept;
    const Index& Value() const noexcept;
    const Manifest& Description() const noexcept;
    // Validated source/operation metadata from the same catalog CAS snapshot.
    // Raw payloads are not opened or pinned here; recovery still requires the
    // store to bind and verify the recorded directory identity.
    const SourceRegistry& Sources() const noexcept;
    // Invalid and moved-from objects return static empty values. References to
    // a valid object's values expire when it is destroyed or replaced.
private:
    struct State;
    std::unique_ptr<State> state_;
    friend ReadResult LoadPublishedIndex(const cache_catalog::Snapshot&,
        const cache_files::Directory&, PublishedIndex&) noexcept;
};

// Requires a valid source and pages directory, including for an empty manifest.
// Acquires exact PageRef identities, verifies real bytes, decodes pages and
// validates all cross-page relationships before publishing output once.
// Every failure preserves output and its existing leases. source may refer to
// output.Source(). Successful output retains all page leases until destruction
// or move replacement. Effect/texture and raw source payloads are not opened.
// Both legacy v1 manifests and composite business roots are accepted; source
// owners are checked against the actual assembled project pages, not counts.
// Invalid denotes invalid arguments; damaged/missing/mismatched stored data is
// RecoveryRequired. Codec Unsupported/Limit/AllocationFailure remain distinct.
// File-layer Io remains Io, including allocation failures hidden by that layer.
// This loads the caller's already-verified snapshot and already-open directory;
// it does not provide an atomic read across directories. A future business
// Store must supply its own lock and pinned catalog read before invoking it.
ReadResult LoadPublishedIndex(const cache_catalog::Snapshot& source,
    const cache_files::Directory& pages, PublishedIndex& output) noexcept;
}
