#pragma once

#include "mikumikustudio/ported_funcs.hpp"
#include "../io/project_relocation.hpp"
#include <filesystem>
#include <vector>

namespace mikumikustudio::project_model {
struct Reference {
    // A legacy byte string can have several valid decodings. Keep the missing
    // spellings until the caller has a successfully loaded manual selection.
    std::vector<std::wstring> missingSpellings;
    bool Confirm(project_relocation::RootMappings& roots, std::wstring_view loaded) const;
    std::wstring exactPath;       // original or inferred path to pass directly to the real loader
    bool tryLegacy = true;        // false with no exactPath means request manual location
};

Reference Locate(const char* wirePath, const std::filesystem::path& projectDirectory,
                 const project_relocation::RootMappings& roots);
bool LoadExact(unsigned char* model, HWND window, const wchar_t* path, D3DRenderer* renderer,
               std::uint8_t language, PhysicsScene* physics, PathResolutionWorkspace& workspace);
}
