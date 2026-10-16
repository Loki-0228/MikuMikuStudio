#pragma once
#include "common.hpp"

namespace mikudancestudio::plugins {
class UiRegistry;
// Process-local writer for one user layout file. Invalid or unknown documents
// are left intact. A separate lock serializes cooperating MMS processes.
class UiPreferencesFile {
    fs::path path_;
    std::string baseline_;
    uint64_t revision_ = 0;
    bool loaded_ = false, existed_ = false, writable_ = false;
public:
    explicit UiPreferencesFile(fs::path path) : path_(std::move(path)) {}
    MmsResult Load(UiRegistry&);
    MmsResult Save(UiRegistry&);
};
}
