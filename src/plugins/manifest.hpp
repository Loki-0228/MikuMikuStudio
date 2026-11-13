#pragma once
#include "json.hpp"
#include <array>
#include <set>

namespace mikumikustudio::plugins {
using Version = std::array<uint32_t, 3>;
Version ParseVersion(const std::string&);
struct ServiceRequirement { std::string id; uint32_t major, minor; };
struct Dependency { std::string id; Version minimum, maximum; };
struct Manifest {
    enum class Kind { Native, LegacyMme, LegacyMmdPlugin } kind = Kind::Native;
    fs::path file, package, entry;
    fs::path legacyHack;
    std::string legacyEntrySha256;
    std::string id, name;
    Version version{}, minWindows{};
    uint32_t major = 1, minor = 0;
    std::vector<std::string> capabilities;
    std::vector<ServiceRequirement> services;
    std::vector<Dependency> dependencies;
    std::vector<fs::path> nativeDependencies;
};
struct DiscoveryConfig { uint32_t maxDepth = 2; std::set<std::string> disabled; };
struct Discovery { DiscoveryConfig config; std::vector<Manifest> packages; std::vector<std::string> diagnostics; };
// Requires every component (including the root) to exist and not be a reparse point.
fs::path PackageFile(const fs::path& root, std::string_view relative);
Manifest ReadManifest(const fs::path&, const Log&);
Discovery Discover(const fs::path& root);
// Duplicate IDs, invalid ranges, missing dependencies and cycles never load.
std::vector<size_t> Resolve(const Discovery&, const Log&);
}
