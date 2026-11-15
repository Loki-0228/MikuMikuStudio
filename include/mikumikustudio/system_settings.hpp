#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace mikumikustudio::system_settings {
enum class Result { Ok, Missing, Invalid, Busy, Io, Conflict };
enum class PluginResult { Ok, Missing, Invalid, Busy, Io, Conflict, Corrupt, Limit };
// Each section is a JSON object. Writes merge object members recursively, so
// modules can update known settings without discarding newer/unknown members.
// Reads leave output untouched on failure. Invalid/unsupported files are never
// replaced. Calls serialize across threads and processes; no UI/GPU dependency.
class Store {
public:
    explicit Store(std::filesystem::path path) : path_(std::move(path)) {}
    Result ReadSection(std::string_view name, std::string& output) const noexcept;
    Result WriteSection(std::string_view name, std::string_view object) const noexcept;
    // Nonempty object mapping section names to objects. Validate every entry,
    // merge against one locked snapshot, and atomically save the entire file.
    // Unknown root fields, other sections and nested members are preserved.
    Result WriteSections(std::string_view sectionsObject) const noexcept;
    // A null expected pointer requires the section to be absent. Otherwise the
    // expected JSON object is compared structurally (member order/space ignored)
    // under the same lock as whole-section replacement. The caller owns unknown
    // fields inside this section. Other sections and root fields are preserved.
    Result CompareExchangeSection(std::string_view name, const std::string* expected,
        std::string_view object) const noexcept;
    // Host-owned plugin registry. The ordinary write APIs cannot modify it.
    // The caller takes manifestId only from the validated package manifest.
    // These calls use the same file lock as all other sections, without waiting.
    PluginResult ReadPlugin(std::string_view manifestId, std::string& output) const noexcept;
    PluginResult CompareMergePlugin(std::string_view manifestId, std::string_view expected,
        std::string_view patch) const noexcept; // empty expected means absent
private:
    Result Write(std::string_view name, std::string_view object, bool compare,
        const std::string* expected, bool multiple = false) const noexcept;
    std::filesystem::path path_;
};
std::filesystem::path Path();
Result ReadSection(std::string_view name, std::string& output) noexcept;
Result WriteSection(std::string_view name, std::string_view object) noexcept;
Result WriteSections(std::string_view sectionsObject) noexcept;
Result CompareExchangeSection(std::string_view name, const std::string* expected,
    std::string_view object) noexcept;
}
