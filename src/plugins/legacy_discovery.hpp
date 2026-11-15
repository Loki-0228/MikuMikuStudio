#pragma once
#include "manifest.hpp"

namespace mikumikustudio::plugins {
// Cache recognition, including negative results, but never execute candidates.
class LegacyDiscovery {
    struct Entry { std::string stamp, hash, profile, error; };
    fs::path root_, cache_;
    std::map<std::string, Entry> previous_, current_;
    Discovery& result_;
public:
    LegacyDiscovery(const fs::path& root, Discovery&, bool useCache);
    void Inspect(const fs::path& file);
    void Save() noexcept;
};
std::string JsonQuote(std::string_view);
}
