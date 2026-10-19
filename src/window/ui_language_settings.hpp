#pragma once
#include <filesystem>
namespace mikumikustudio::language_preferences {
void LoadFrom(const std::filesystem::path& settings,const std::filesystem::path& legacy);
void SaveTo(const std::filesystem::path& settings);
}
