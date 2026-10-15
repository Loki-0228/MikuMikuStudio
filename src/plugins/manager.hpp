#pragma once
#include "ui_registry.hpp"
#include "loader_win32.hpp"

namespace mikudancestudio::plugins {
struct PluginStatus { std::string id, stage, error; fs::path package; };
class Manager {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Manager(fs::path userRoot, Log log, std::function<void()> uiChanged = {}, bool forceFallback = false);
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    void Load(const fs::path& pluginsRoot);
    void Drain();
    bool Stop();
    void Event(uint32_t kind);
    UiRegistry& Ui();
    std::vector<PluginStatus> Status() const;
};
}
