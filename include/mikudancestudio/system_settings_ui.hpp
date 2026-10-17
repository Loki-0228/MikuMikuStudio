#pragma once
namespace mikudancestudio {
class MMDApp;
namespace system_settings_ui {
constexpr unsigned kCommand = 32812;
struct HistoryPreferences {
    bool recordAdjustments=false;
    unsigned steps=32;
    unsigned memoryMiB=64;
};
HistoryPreferences HistoryPreference() noexcept;
bool RecordAdjustmentsPreference() noexcept;
void EnsureItem(MMDApp*);
bool HandleCommand(MMDApp*, unsigned);
}
}
