#pragma once
#include "../plugins/json.hpp"
#include "mikumikustudio/system_settings.hpp"
#include <cmath>
#include <climits>
#include <cstdlib>
#include <locale.h>

namespace mikumikustudio::settings_json {
using plugins::Json;
inline void Quote(std::string& out, std::string_view value) {
    out += '"';
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    out += '"';
}
inline std::string Quote(std::string_view value) { std::string out; Quote(out, value); return out; }
inline std::string Quote(std::wstring_view value) { return Quote(plugins::Utf8(value)); }
inline void Serialize(std::string& output, const Json& value) {
    switch (value.kind) {
    case Json::Null: output += "null"; break;
    case Json::Boolean: case Json::Number: output += value.text; break;
    case Json::StringValue: Quote(output, value.text); break;
    case Json::Array: {
        output += '['; bool first = true;
        for (const auto& item : value.array) { if (!first) output += ','; first = false; Serialize(output, item); }
        output += ']'; break;
    }
    case Json::Object: {
        output += '{'; bool first = true;
        for (const auto& item : value.object) {
            if (!first) output += ',';
            first = false;
            Quote(output, item.first); output += ':'; Serialize(output, item.second);
        }
        output += '}'; break;
    }
    }
}
inline std::string Serialize(const Json& value) { std::string output; Serialize(output, value); return output; }
inline unsigned UInt(const Json& object, const char* key, unsigned fallback) {
    const auto found = object.object.find(key);
    return found == object.object.end() ? fallback : found->second.UInt();
}
inline bool Bool(const Json& object, const char* key, bool fallback) {
    const auto found = object.object.find(key);
    if (found == object.object.end()) return fallback;
    if (found->second.kind != Json::Boolean) throw std::invalid_argument("expected boolean setting");
    return found->second.text == "true";
}
inline std::string String(const Json& object, const char* key, std::string fallback = {}) {
    const auto found = object.object.find(key);
    return found == object.object.end() ? fallback : found->second.String();
}
inline Json Object(std::string_view text) {
    auto value = Json::Parse(text);
    if (value.kind != Json::Object) throw std::invalid_argument("expected settings object");
    return value;
}
inline const char* Bool(bool value) { return value ? "true" : "false"; }
inline int Integer(const Json& object, const char* name, int fallback) {
    auto found = object.object.find(name);
    if (found == object.object.end() || found->second.kind != Json::Number) return fallback;
    const auto& text = found->second.text;
    if (text.empty() || text.find_first_not_of("-0123456789") != std::string::npos) return fallback;
    char* end = nullptr;
    const auto value = std::strtoll(text.c_str(), &end, 10);
    return end && !*end && value >= INT_MIN && value <= INT_MAX ? static_cast<int>(value) : fallback;
}
inline float Float(const Json& object, const char* name, float fallback) {
    auto found = object.object.find(name);
    if (found == object.object.end() || found->second.kind != Json::Number) return fallback;
    const auto& text = found->second.text;
    char* end = nullptr;
    // The JSON reader already validated the decimal grammar. Use the CRT's
    // float conversion with explicit JSON punctuation, retaining finite underflow.
#ifdef _WIN32
    const auto locale = _create_locale(LC_NUMERIC, "C");
    if (!locale) return fallback;
    const float value = _strtof_l(text.c_str(), &end, locale);
    _free_locale(locale);
#else
    const auto locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    if (!locale) return fallback;
    const float value = strtof_l(text.c_str(), &end, locale);
    freelocale(locale);
#endif
    return end != text.c_str() && end == text.c_str() + text.size() && std::isfinite(value) ? value : fallback;
}
inline bool Read(const char* section, Json& out, system_settings::Result& result) {
    std::string json; result = system_settings::ReadSection(section, json);
    if (result != system_settings::Result::Ok) return false;
    try { out = Json::Parse(json); return out.kind == Json::Object; } catch (...) { return false; }
}
}
