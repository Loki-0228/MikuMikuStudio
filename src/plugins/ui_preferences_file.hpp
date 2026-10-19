#pragma once
#include "common.hpp"

namespace mikumikustudio::plugins {
class UiRegistry;
// Process-local writer for one user layout file. Invalid or unknown documents
// are left intact. A separate lock serializes cooperating MMS processes.
class UiPreferencesFile {
    fs::path path_;
    fs::path settingsPath_;
    std::string baseline_;
    uint64_t revision_ = 0;
    bool loaded_ = false, existed_ = false, writable_ = false;
    bool unified_ = false, sectionExisted_ = false;
public:
    explicit UiPreferencesFile(fs::path path, bool unified = false, fs::path settingsPath = {})
        : path_(std::move(path)), settingsPath_(std::move(settingsPath)), unified_(unified) {}
    MmsResult Load(UiRegistry&);
    MmsResult Save(UiRegistry&);
};
}
