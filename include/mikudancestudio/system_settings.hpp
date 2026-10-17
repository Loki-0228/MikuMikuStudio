#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace mikudancestudio::system_settings {
enum class Result { Ok, Missing, Invalid, Busy, Io, Conflict };
// Each section is a JSON object. Writes merge object members recursively, so
// modules can update known settings without discarding newer/unknown members.
// Reads leave output untouched on failure. Invalid/unsupported files are never
// replaced. Calls serialize across threads and processes; no UI/GPU dependency.
class Store {
public:
    explicit Store(std::filesystem::path path) : path_(std::move(path)) {}
    Result ReadSection(std::string_view name, std::string& output) const noexcept;
    Result WriteSection(std::string_view name, std::string_view object) const noexcept;
    // A null expected pointer requires the section to be absent. Otherwise the
    // expected JSON object is compared structurally (member order/space ignored)
    // under the same lock as whole-section replacement. The caller owns unknown
    // fields inside this section. Other sections and root fields are preserved.
    Result CompareExchangeSection(std::string_view name, const std::string* expected,
        std::string_view object) const noexcept;
private:
    Result Write(std::string_view name, std::string_view object, bool compare,
        const std::string* expected) const noexcept;
    std::filesystem::path path_;
};
std::filesystem::path Path();
Result ReadSection(std::string_view name, std::string& output) noexcept;
Result WriteSection(std::string_view name, std::string_view object) noexcept;
Result CompareExchangeSection(std::string_view name, const std::string* expected,
    std::string_view object) noexcept;
}
