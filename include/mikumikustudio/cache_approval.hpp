#pragma once
#include "mikumikustudio/cache_files.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace mikumikustudio::cache_catalog {
enum class ApprovalArea : std::size_t {
    Manager, Video, Control, Catalog, Pages, Raw, Snapshots, Commits, Operations,
    Count
};

// Facts supplied by the trusted common-budget admission path. Merely creating,
// encoding or decoding this value does not grant quota or authorize recovery.
// Directory identities must describe nine distinct directories on one volume.
// A zero volume serial is valid; identity comparisons must not use it as a
// missing-value sentinel.
struct PublicationApproval {
    std::array<cache_files::DirectoryIdentity,
        static_cast<std::size_t>(ApprovalArea::Count)> directories{};
    std::uint64_t allocationUnit = 0;
    std::uint64_t physicalBytes = 0;
    std::uint64_t legacyReservedBytes = 0;
    std::uint64_t beforeRawResidual = 0;
    std::uint64_t beforeMetadata = 0;
    std::uint64_t afterRawResidual = 0;
    std::uint64_t afterMetadata = 0;
    std::uint64_t policyLimitBytes = 0;
    std::uint64_t approvedPeak = 0;
    bool policyEnabled = false;
};
}
