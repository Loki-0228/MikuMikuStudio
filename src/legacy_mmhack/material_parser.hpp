#pragma once

#include "state.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace mme_re::mmhack {

enum class MaterialFileFormat { pmd, pmx };
struct MaterialTable {
  MaterialFileFormat format = MaterialFileFormat::pmd;
  std::vector<MaterialNames> materials;
};

// Restores FUN_18000e020 / FUN_18000bbf0 / FUN_18000c960. The destination is
// replaced only after the complete material table has been read successfully.
// File paths are UTF-16 on Windows. PMX text follows its encoding byte; PMD and
// BOM-less companion text use CP_ACP, matching FUN_18000b7c0.
bool parse_material_table(const std::filesystem::path& model_path,
    MaterialTable& destination, std::string& error);

// The byte-oriented entry point permits independent synthetic verification.
// companion_text is optional PMD <model>.txt content, not a second model file.
bool parse_material_table_bytes(const std::vector<std::uint8_t>& model_bytes,
    const std::filesystem::path& model_path,
    const std::vector<std::uint8_t>* companion_text,
    MaterialTable& destination, std::string& error);

} // namespace mme_re::mmhack
