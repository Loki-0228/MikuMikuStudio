#pragma once

#include "mikumikustudio/model_layout.hpp"
#include "mikumikustudio/subrecord_layout.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <stdexcept>

namespace mikumikustudio {
// Runtime references, not authored fields, file content, or resource identities.
struct MeshSceneMaterialReferences {
    std::array<wchar_t,280> texturePath{}, spherePath{};
    std::array<wchar_t,512> toonPath{};
    std::uint8_t physicsMode = 0, toonReference = 0, sphereMode = 0, flags = 0;
};
struct MeshSceneToonTableReferences {
    std::array<wchar_t,256> modelDirectory{};
    std::array<std::array<char,100>,10> fileNames{};
};
namespace mesh_scene_references_detail {
template<std::size_t N> inline void CopyPath(std::array<wchar_t,N>& out, const wchar_t (&in)[N]) {
    const auto end = std::find(std::begin(in), std::end(in), L'\0');
    if (end == std::end(in)) throw std::invalid_argument("unterminated material path");
    std::copy(std::begin(in), end, out.begin());
}
}
// Stage all fields before writing output, including when validation fails.
inline void CaptureMeshSceneMaterialReferences(const mdl::ModelMaterialRecord& source,
    std::uint8_t mode, MeshSceneMaterialReferences& out) {
    MeshSceneMaterialReferences value{};
    mesh_scene_references_detail::CopyPath(value.texturePath, source.texturePath);
    mesh_scene_references_detail::CopyPath(value.spherePath, source.spherePath);
    mesh_scene_references_detail::CopyPath(value.toonPath, source.toonPath);
    value.physicsMode=mode; value.toonReference=source.toonReference;
    value.sphereMode=source.sphereMode; value.flags=source.flags;
    out=value;
}
inline void CaptureMeshSceneToonTableReferences(const mdl::ModelRecord& source,
    MeshSceneToonTableReferences& out) {
    MeshSceneToonTableReferences value{};
    mesh_scene_references_detail::CopyPath(value.modelDirectory, source.modelDirectory);
    for (std::size_t t=0;t<value.fileNames.size();++t)
        std::copy_n(source.pmdToonFileNames[t],value.fileNames[t].size(),value.fileNames[t].begin());
    out=value;
}
}
