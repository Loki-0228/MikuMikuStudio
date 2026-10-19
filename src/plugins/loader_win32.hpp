#pragma once
#include "manifest.hpp"

namespace mikumikustudio::plugins {
struct PeInfo { std::vector<std::string> imports; uint16_t subsystemMajor = 0, subsystemMinor = 0; };
PeInfo InspectPe(const fs::path&);
std::string Sha256(const fs::path&);
Version WindowsVersion();
struct LoadedModule {
    HMODULE handle = nullptr;
    fs::path path;
    std::string hash;
    ~LoadedModule() { if (handle) FreeLibrary(handle); }
};
struct LoadedPackage {
    std::vector<std::shared_ptr<LoadedModule>> modules;
    HMODULE entry = nullptr;
    LoadedPackage() = default;
    LoadedPackage(LoadedPackage&&) = default;
    LoadedPackage& operator=(LoadedPackage&&) = default;
    ~LoadedPackage() { while (!modules.empty()) modules.pop_back(); }
    void Pin() noexcept; // Retain code until process exit when stop cannot drain.
};
class NativeLoader {
    std::map<std::wstring, std::weak_ptr<LoadedModule>> modules_;
    bool safeSearch_;
    fs::path system_;
public:
    explicit NativeLoader(bool forceFallback = false);
    LoadedPackage Load(const Manifest&, const Log&);
};
}
