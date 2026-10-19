#pragma once
#include <string>
#include <string_view>

namespace mikumikustudio::bake {
struct Settings {
    float erp = 0.2f;
    float cfm = 0.f;
    int substeps = 1;
    int rateMultiplier = 1;
    int baseRateHz = 60;
    std::wstring name;
};
enum class SettingsError {
    None, InvalidErp, InvalidCfm, InvalidSubsteps, InvalidMultiplier,
    InvalidBaseRate, InvalidName, InvalidJson, UnsupportedVersion,
    MissingField, IoRead, IoWrite, TooLarge
};
Settings BulletDefaults();
bool ValidateSettings(const Settings&, SettingsError&);
bool DecodePreset(std::string_view, Settings&, SettingsError&);
std::string EncodePreset(const Settings&);
bool LoadPreset(const wchar_t*, Settings&, SettingsError&);
bool SavePreset(const wchar_t*, const Settings&, SettingsError&);
}