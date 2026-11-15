#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mikumikustudio::plugin_settings_merge {
enum class Result { Ready, Unchanged, Conflict, Invalid, Corrupt, Limit };

namespace detail {
inline constexpr char Section[] = "pluginSettings";
inline constexpr std::size_t MaximumBytes = 1024 * 1024; // whole settings document
inline constexpr std::size_t MaximumPluginBytes = 64 * 1024;
inline constexpr std::size_t MaximumRegistryBytes = 512 * 1024;

inline bool RegularManifestId(std::string_view id) {
    // Match plugins::ValidId and the manifest's mms / mms.* reservation.
    return !id.empty() && id.size() <= 128 && id != "mms" && id.substr(0, 4) != "mms." &&
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") == std::string_view::npos;
}

template<class Json>
const Json* Find(const Json* object, const std::string& key) {
    if (!object || object->kind != Json::Object) return nullptr;
    const auto it = object->object.find(key);
    return it == object->object.end() ? nullptr : &it->second;
}

template<class Json>
bool MergeObject(Json& target, const Json& update) {
    for (const auto& [key, value] : update.object) {
        auto [it, inserted] = target.object.try_emplace(key, value);
        if (inserted) continue;
        auto& existing = it->second;
        if (existing.kind == Json::Object) {
            if (value.kind != Json::Object || !MergeObject(existing, value)) return false;
        } else {
            existing = value;
        }
    }
    return true;
}
} // namespace detail

template<class Json>
Result Lookup(const Json& document, std::string_view manifestId, const Json*& current) {
    if (!detail::RegularManifestId(manifestId)) return Result::Invalid;
    const auto* schema = detail::Find(&document, "schemaVersion");
    const auto* sections = detail::Find(&document, "sections");
    if (!schema || schema->kind != Json::Number || schema->text != "1" ||
        !sections || sections->kind != Json::Object) return Result::Corrupt;
    const auto* registry = detail::Find(sections, detail::Section);
    if (registry && registry->kind != Json::Object) return Result::Corrupt;
    const auto* entry = detail::Find(registry, std::string(manifestId));
    if (entry && entry->kind != Json::Object) return Result::Corrupt;
    current = entry;
    return Result::Ready;
}

// Internal planning policy only. The caller supplies the loaded (or empty
// version-1) document, the exact ID from the owning manifest, and the parsed
// object patch and baseline. A null baseline expects the entry to be absent.
// Parse the inputs with the bounded settings codec before calling. Serialize is
// the existing settings canonical serializer. Compare happens within just the
// owning plugin's entry (including number spelling), not against its parent.
//
// Patch objects preserve omitted keys. Arrays are atomic values and null is a
// literal, never a delete operation. An existing object cannot be replaced by
// any non-object. Only Ready replaces output, with validated bytes for the
// whole document (including newline). No input or file is modified here.
template<class Json, class Serialize>
Result Plan(const Json& document, std::string_view manifestId, const Json* baseline,
    const Json& update, Serialize serialize, std::string& output) {
    if (update.kind != Json::Object || (baseline && baseline->kind != Json::Object)) return Result::Invalid;

    const Json* current = nullptr;
    const auto lookup = Lookup(document, manifestId, current);
    if (lookup != Result::Ready) return lookup;
    if ((current == nullptr) != (baseline == nullptr)) return Result::Conflict;
    const auto before = current ? serialize(*current) : std::string{};
    if (baseline && before != serialize(*baseline)) return Result::Conflict;
    if (update.object.empty()) return Result::Unchanged;

    auto merged = current ? *current : Json{};
    merged.kind = Json::Object;
    if (!detail::MergeObject(merged, update)) return Result::Invalid;
    const auto after = serialize(merged);
    if (current && before == after) return Result::Unchanged;
    if (after.size() > detail::MaximumPluginBytes) return Result::Limit;

    auto candidate = document;
    auto& destination = candidate.object.at("sections").object[detail::Section];
    destination.kind = Json::Object;
    destination.object.insert_or_assign(std::string(manifestId), std::move(merged));
    if (serialize(destination).size() > detail::MaximumRegistryBytes) return Result::Limit;
    auto bytes = serialize(candidate);
    if (bytes.size() >= detail::MaximumBytes) return Result::Limit; // include newline
    bytes += '\n';
    try {
        // Check the combined depth and node count, not just the input limits.
        Json::Parse(bytes, detail::MaximumBytes);
    } catch (const std::invalid_argument&) {
        return Result::Limit;
    }
    output.swap(bytes);
    return Result::Ready;
}
} // namespace mikumikustudio::plugin_settings_merge
