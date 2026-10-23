#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "../plugins/json.hpp"
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace mikumikustudio::cache_manager {
namespace {
using plugins::Json;

// One settings-file section owns this policy. Unknown members of that object
// belong to newer builds, so a commit must carry them through unchanged.
constexpr char SectionName[] = "cacheManager";
constexpr std::uint32_t MinimumLimitMiB = 64;
constexpr std::uint32_t MaximumLimitMiB = 102400;
constexpr std::uint32_t MinimumRecentProjects = 1;
constexpr std::uint32_t MaximumRecentProjects = 100;

// Mirrors the canonical spelling system_settings compares with: sorted object
// members, no whitespace, number spellings preserved. The snapshot text is
// then a stable baseline even though the store canonicalizes it again.
void Quote(std::string& output, const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    output += '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { output += '\\'; output += char(c); }
        else if (c < 32) { output += "\\u00"; output += hex[c >> 4]; output += hex[c & 15]; }
        else output += char(c);
    }
    output += '"';
}

void Serialize(std::string& output, const Json& value) {
    switch (value.kind) {
    case Json::Null: output += "null"; break;
    case Json::Boolean:
    case Json::Number: output += value.text; break;
    case Json::StringValue: Quote(output, value.text); break;
    case Json::Array: {
        output += '[';
        bool first = true;
        for (const auto& item : value.array) {
            if (!first) output += ',';
            first = false;
            Serialize(output, item);
        }
        output += ']';
        break;
    }
    case Json::Object: {
        output += '{';
        bool first = true;
        for (const auto& item : value.object) {
            if (!first) output += ',';
            first = false;
            Quote(output, item.first);
            output += ':';
            Serialize(output, item.second);
        }
        output += '}';
        break;
    }
    }
}

std::string Serialize(const Json& value) {
    std::string output;
    Serialize(output, value);
    return output;
}

Json BooleanJson(bool value) {
    Json json;
    json.kind = Json::Boolean;
    json.text = value ? "true" : "false";
    return json;
}

Json NumberJson(std::uint32_t value) {
    Json json;
    json.kind = Json::Number;
    json.text = std::to_string(value);
    return json;
}

bool BooleanValue(const Json& value, bool& output) noexcept {
    if (value.kind != Json::Boolean) return false;
    if (value.text == "true") { output = true; return true; }
    if (value.text == "false") { output = false; return true; }
    return false;
}

// Json::UInt accepts decimal digits only, so negatives, fractions and
// exponents are rejected instead of being rounded into a policy value. Only
// its invalid_argument marks the field Invalid; allocation and any other
// failure propagate to the caller's IO mapping.
bool UIntValue(const Json& value, std::uint32_t& output) {
    if (value.kind != Json::Number) return false;
    try {
        output = value.UInt();
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

// Not noexcept: the member lookups allocate, and allocation failure must reach
// the caller's IO mapping instead of terminating the process.
bool Decode(const Json& section, Configuration& output) {
    const auto& object = section.object;
    const auto enabled = object.find("enabled");
    if (enabled != object.end() && !BooleanValue(enabled->second, output.enabled)) return false;
    const auto automatic = object.find("automaticCleanup");
    if (automatic != object.end() && !BooleanValue(automatic->second, output.automaticCleanup)) return false;
    const auto limit = object.find("limitMiB");
    if (limit != object.end() && !UIntValue(limit->second, output.limitMiB)) return false;
    const auto recent = object.find("recentProjects");
    if (recent != object.end() && !UIntValue(recent->second, output.recentProjects)) return false;
    return Valid(output);
}

bool Usable(const std::filesystem::path& root, const std::filesystem::path& settings) noexcept {
    return !root.empty() && !settings.empty() && root.is_absolute() && settings.is_absolute();
}

Result Map(system_settings::Result result) noexcept {
    switch (result) {
    case system_settings::Result::Ok: return Result::Ok;
    case system_settings::Result::Missing: return Result::Io;
    case system_settings::Result::Invalid: return Result::Invalid;
    case system_settings::Result::Busy: return Result::Busy;
    case system_settings::Result::Io: return Result::Io;
    case system_settings::Result::Conflict: return Result::Conflict;
    }
    return Result::Io;
}
}  // namespace

bool Valid(const Configuration& value) noexcept {
    return value.limitMiB >= MinimumLimitMiB && value.limitMiB <= MaximumLimitMiB &&
        value.recentProjects >= MinimumRecentProjects && value.recentProjects <= MaximumRecentProjects;
}

void Snapshot::Swap(Snapshot& other) noexcept {
    std::swap(value_.enabled, other.value_.enabled);
    std::swap(value_.automaticCleanup, other.value_.automaticCleanup);
    std::swap(value_.limitMiB, other.value_.limitMiB);
    std::swap(value_.recentProjects, other.value_.recentProjects);
    root_.swap(other.root_);
    settings_.swap(other.settings_);
    section_.swap(other.section_);
    std::swap(present_, other.present_);
    std::swap(valid_, other.valid_);
}

Store::Store(std::filesystem::path absoluteCacheRoot, std::filesystem::path absoluteSettingsFile)
    : root_(absoluteCacheRoot.lexically_normal()),
      settings_(absoluteSettingsFile.lexically_normal()) {}

Result Store::Read(Snapshot& output) const noexcept {
    try {
        if (!Usable(root_, settings_)) return Result::Invalid;
        std::string body;
        const auto result = system_settings::Store(settings_).ReadSection(SectionName, body);
        if (result == system_settings::Result::Missing) {
            // Nothing persisted yet: defaults are the policy, but no section is
            // claimed for a compare-and-swap until a caller commits one.
            Snapshot candidate;
            candidate.value_ = Configuration{};
            candidate.root_ = root_;
            candidate.settings_ = settings_;
            candidate.present_ = false;
            candidate.valid_ = true;
            output.Swap(candidate);
            return Result::Ok;
        }
        if (result != system_settings::Result::Ok) return Map(result);
        Json section;
        try {
            section = Json::Parse(body);
        } catch (const std::invalid_argument&) {
            return Result::Invalid;
        }
        if (section.kind != Json::Object) return Result::Invalid;
        Snapshot candidate;
        candidate.value_ = Configuration{};
        if (!Decode(section, candidate.value_)) return Result::Invalid;
        candidate.root_ = root_;
        candidate.settings_ = settings_;
        candidate.section_ = Serialize(section);
        candidate.present_ = true;
        candidate.valid_ = true;
        output.Swap(candidate);
        return Result::Ok;
    } catch (...) {
        return Result::Io;
    }
}

Result Store::Commit(const Snapshot& expected, const Configuration& desired,
    Snapshot& output) const noexcept {
    try {
        if (!expected.IsValid()) return Result::Invalid;
        if (!Usable(root_, settings_)) return Result::Invalid;
        // A snapshot of another installation must never authorize this write.
        if (expected.root_ != root_ || expected.settings_ != settings_) return Result::Invalid;
        if (!Valid(desired)) return Result::Invalid;
        Json section;
        if (expected.present_) {
            try {
                section = Json::Parse(expected.section_);
            } catch (const std::invalid_argument&) {
                return Result::Invalid;
            }
            if (section.kind != Json::Object) return Result::Invalid;
        } else {
            section.kind = Json::Object;
        }
        // Replace only the four owned members; unknown nested values, arrays
        // and escaped strings inside the section survive the round trip.
        section.object["enabled"] = BooleanJson(desired.enabled);
        section.object["automaticCleanup"] = BooleanJson(desired.automaticCleanup);
        section.object["limitMiB"] = NumberJson(desired.limitMiB);
        section.object["recentProjects"] = NumberJson(desired.recentProjects);
        // Build every allocation, the serialized payload and the candidate
        // snapshot before touching the file, so success has nothing left to
        // fail on.
        const auto serialized = Serialize(section);
        Snapshot candidate;
        candidate.value_ = desired;
        candidate.root_ = root_;
        candidate.settings_ = settings_;
        candidate.section_ = serialized;
        candidate.present_ = true;
        candidate.valid_ = true;
        const std::string* expectedSection = expected.present_ ? &expected.section_ : nullptr;
        const auto result = system_settings::Store(settings_).CompareExchangeSection(
            SectionName, expectedSection, serialized);
        if (result != system_settings::Result::Ok) return Map(result);
        output.Swap(candidate);
        return Result::Ok;
    } catch (...) {
        return Result::Io;
    }
}

std::filesystem::path Root() noexcept {
    if constexpr (build_profile::original) {
        return {};
    } else {
        try {
            return local_data::File(L"cache");
        } catch (...) {
            return {};
        }
    }
}

Result ReadCurrent(Snapshot& output) noexcept {
    if constexpr (build_profile::original) {
        return Result::Disabled;
    } else {
        try {
            return Store(Root(), system_settings::Path()).Read(output);
        } catch (...) {
            return Result::Io;
        }
    }
}

Result Configure(const Snapshot& expected, const Configuration& desired, Snapshot& output) noexcept {
    if constexpr (build_profile::original) {
        return Result::Disabled;
    } else {
        try {
            return Store(Root(), system_settings::Path()).Commit(expected, desired, output);
        } catch (...) {
            return Result::Io;
        }
    }
}

bool Enabled() noexcept {
    if constexpr (build_profile::original) {
        return false;
    } else {
        try {
            Snapshot snapshot;
            return ReadCurrent(snapshot) == Result::Ok && snapshot.Value().enabled;
        } catch (...) {
            return false;
        }
    }
}
}  // namespace mikumikustudio::cache_manager
