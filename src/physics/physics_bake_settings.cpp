// MikuMikuStudio: physics bake parameter presets (UTF-8 JSON), standalone module.
//
// The wire format is the one fixed in docs/guides/physics-baking.md:
//
//     {
//       "format": "MikuMikuStudioPhysicsPreset",
//       "schemaVersion": 1,
//       "name": "preset name",
//       "erp": 0.2,
//       "cfm": 0.0,
//       "substeps": 1,
//       "simulationRateMultiplier": 1,
//       "baseSimulationRateHz": 60
//     }
//
// Contract implemented here (header: include/mikudancestudio/physics_bake_settings.hpp):
//
//   * All eight keys are mandatory and no other key is accepted.  Duplicate
//     keys, unknown keys, wrong value types, malformed numbers, numbers whose
//     magnitude overflows the double range, substeps / rateMultiplier /
//     baseSimulationRateHz without an exact integer value, trailing garbage
//     after the closing brace and an unsupported schemaVersion are rejected.
//   * The text is strict UTF-8 with an optional BOM.  Strings support the
//     JSON escapes including \uXXXX with surrogate pairs.  Raw control
//     characters inside strings and invalid UTF-8 byte sequences are
//     rejected as InvalidJson.
//   * Error precedence:
//       1. input longer than 64 KiB                     -> TooLarge
//       2. JSON syntax / UTF-8 / duplicate / unknown key
//          / wrong value type                            -> InvalidJson,
//          except a syntactically valid number that cannot be represented,
//          which reports the owning field's error (see FieldError)
//       3. missing key                                   -> MissingField
//       4. format identifier mismatch                    -> InvalidJson
//       5. schemaVersion != 1                            -> UnsupportedVersion
//       6. value validation in Settings order: erp, cfm, substeps,
//          rateMultiplier, baseRateHz, name
//   * A failed DecodePreset / LoadPreset never touches the caller's
//     Settings; a successful one replaces every field.
//   * EncodePreset writes classic-locale numbers with max_digits10 so float
//     values survive the round trip exactly, and escapes the name into valid
//     UTF-8.  Invalid settings produce an empty string.
//   * SavePreset validates first, writes a unique temp file next to the
//     target, then replaces the target atomically with MoveFileExW
//     (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).  On failure only
//     the temp file this call created is removed: an existing preset keeps
//     its old content and no foreign file is ever deleted.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "btBulletDynamicsCommon.h"

#include "mikudancestudio/physics_bake_settings.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace mikudancestudio::bake {
namespace {

constexpr std::size_t kMaxPresetBytes = 64u * 1024u;
constexpr std::size_t kMaxNameCodePoints = 128u;
constexpr double kErpMin = 0.0;
constexpr double kErpMax = 1.0;
constexpr double kCfmMin = 0.0;
constexpr double kCfmMax = 1000000.0;
constexpr int kSubstepsMin = 1;
constexpr int kSubstepsMax = 16;
constexpr int kMultiplierMin = 1;
constexpr int kMultiplierMax = 16;
constexpr int kBaseRateA = 30;
constexpr int kBaseRateB = 60;
constexpr double kSchemaVersion = 1.0;
constexpr const wchar_t* kFormatId = L"MikuMikuStudioPhysicsPreset";
constexpr const wchar_t* kLegacyFormatId = L"MikuDanceStudioPhysicsPreset";
constexpr const wchar_t* kDefaultName = L"Bullet defaults";

// The eight keys of the format, in the canonical order used for the
// missing-field check.
enum class Field : int {
    Format, SchemaVersion, Name, Erp, Cfm, Substeps, RateMultiplier, BaseRateHz, Count, Unknown
};

constexpr Field kFieldOrder[] = {
    Field::Format, Field::SchemaVersion, Field::Name, Field::Erp,
    Field::Cfm, Field::Substeps, Field::RateMultiplier, Field::BaseRateHz};

SettingsError FieldError(Field field) {
    switch (field) {
        case Field::SchemaVersion: return SettingsError::UnsupportedVersion;
        case Field::Erp: return SettingsError::InvalidErp;
        case Field::Cfm: return SettingsError::InvalidCfm;
        case Field::Substeps: return SettingsError::InvalidSubsteps;
        case Field::RateMultiplier: return SettingsError::InvalidMultiplier;
        case Field::BaseRateHz: return SettingsError::InvalidBaseRate;
        default: return SettingsError::InvalidJson;
    }
}

Field LookupField(const std::wstring& key) {
    if (key == L"format") return Field::Format;
    if (key == L"schemaVersion") return Field::SchemaVersion;
    if (key == L"name") return Field::Name;
    if (key == L"erp") return Field::Erp;
    if (key == L"cfm") return Field::Cfm;
    if (key == L"substeps") return Field::Substeps;
    if (key == L"simulationRateMultiplier") return Field::RateMultiplier;
    if (key == L"baseSimulationRateHz") return Field::BaseRateHz;
    return Field::Unknown;
}

// ---- UTF-16 helpers ------------------------------------------------------

// Appends one code point; on 16-bit wchar_t values beyond the BMP become a
// surrogate pair.
void AppendCodePoint(std::wstring& out, unsigned codePoint) {
    if (sizeof(wchar_t) == 2 && codePoint > 0xFFFFu) {
        const unsigned rest = codePoint - 0x10000u;
        out.push_back(static_cast<wchar_t>(0xD800u + (rest >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00u + (rest & 0x3FFu)));
        return;
    }
    out.push_back(static_cast<wchar_t>(codePoint));
}

// Walks one UTF-16 code point starting at index.  Returns false on an
// unpaired surrogate or a code point outside the Unicode range.
bool NextCodePoint(const std::wstring& text, std::size_t& index, unsigned& codePoint) {
    const unsigned first = static_cast<unsigned>(text[index]);
    if (sizeof(wchar_t) == 2) {
        if (first >= 0xD800u && first <= 0xDBFFu) {
            if (index + 1 >= text.size()) return false;
            const unsigned second = static_cast<unsigned>(text[index + 1]);
            if (second < 0xDC00u || second > 0xDFFFu) return false;
            codePoint = 0x10000u + ((first - 0xD800u) << 10) + (second - 0xDC00u);
            index += 2;
            return true;
        }
        if (first >= 0xDC00u && first <= 0xDFFFu) return false;
        codePoint = first;
        ++index;
        return true;
    }
    codePoint = first;
    ++index;
    return codePoint <= 0x10FFFFu && !(codePoint >= 0xD800u && codePoint <= 0xDFFFu);
}

// A name is at most 128 Unicode code points, without control characters and
// without invalid UTF-16.
bool NameIsValid(const std::wstring& name) {
    std::size_t index = 0;
    std::size_t codePoints = 0;
    while (index < name.size()) {
        unsigned codePoint = 0;
        if (!NextCodePoint(name, index, codePoint)) return false;
        if (codePoint <= 0x1Fu || (codePoint >= 0x7Fu && codePoint <= 0x9Fu)) return false;
        if (++codePoints > kMaxNameCodePoints) return false;
    }
    return true;
}

// ---- JSON scanner (flat object, known fields only) -----------------------

struct Scanner {
    std::string_view text;
    std::size_t pos = 0;
};

bool IsJsonWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void SkipWhitespace(Scanner& s) {
    while (s.pos < s.text.size() && IsJsonWhitespace(s.text[s.pos])) ++s.pos;
}

// Strict UTF-8 decode of one character.  Rejects truncated sequences,
// overlong forms, encoded surrogate code points and values above U+10FFFF.
bool DecodeUtf8Char(const unsigned char* bytes, std::size_t available,
                    std::size_t& length, unsigned& codePoint) {
    const unsigned char lead = bytes[0];
    if (lead < 0x80u) {
        length = 1;
        codePoint = lead;
        return true;
    }
    if (lead < 0xC2u || lead > 0xF4u) return false;
    if (lead < 0xE0u) {
        length = 2;
        codePoint = lead & 0x1Fu;
    } else if (lead < 0xF0u) {
        length = 3;
        codePoint = lead & 0x0Fu;
    } else {
        length = 4;
        codePoint = lead & 0x07u;
    }
    if (available < length) return false;
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = bytes[i];
        if ((next & 0xC0u) != 0x80u) return false;
        codePoint = (codePoint << 6) | (next & 0x3Fu);
    }
    if (length == 2 && codePoint < 0x80u) return false;
    if (length == 3 && codePoint < 0x800u) return false;
    if (length == 3 && codePoint >= 0xD800u && codePoint <= 0xDFFFu) return false;
    if (length == 4 && codePoint < 0x10000u) return false;
    return codePoint <= 0x10FFFFu;
}

bool ScanHex4(Scanner& s, unsigned& value) {
    if (s.text.size() - s.pos < 4) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = s.text[s.pos++];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
        else return false;
    }
    return true;
}

// Scans a JSON string into UTF-16.  Malformed escapes, raw control
// characters and invalid UTF-8 are JSON errors; a lone \uXXXX surrogate is
// kept as a code unit so that name validation reports InvalidName.
bool ScanJsonString(Scanner& s, std::wstring& out) {
    out.clear();
    if (s.pos >= s.text.size() || s.text[s.pos] != '"') return false;
    ++s.pos;
    for (;;) {
        if (s.pos >= s.text.size()) return false;
        const unsigned char c = static_cast<unsigned char>(s.text[s.pos]);
        if (c == '"') {
            ++s.pos;
            return true;
        }
        if (c < 0x20u) return false;  // raw control characters are illegal in JSON strings
        if (c == '\\') {
            ++s.pos;
            if (s.pos >= s.text.size()) return false;
            const char escaped = s.text[s.pos++];
            switch (escaped) {
                case '"': out.push_back(L'"'); break;
                case '\\': out.push_back(L'\\'); break;
                case '/': out.push_back(L'/'); break;
                case 'b': out.push_back(L'\b'); break;
                case 'f': out.push_back(L'\f'); break;
                case 'n': out.push_back(L'\n'); break;
                case 'r': out.push_back(L'\r'); break;
                case 't': out.push_back(L'\t'); break;
                case 'u': {
                    unsigned unit = 0;
                    if (!ScanHex4(s, unit)) return false;
                    if (unit >= 0xD800u && unit <= 0xDBFFu &&
                        s.text.size() - s.pos >= 2 && s.text[s.pos] == '\\' && s.text[s.pos + 1] == 'u') {
                        Scanner follow = s;
                        follow.pos += 2;
                        unsigned low = 0;
                        if (ScanHex4(follow, low) && low >= 0xDC00u && low <= 0xDFFFu) {
                            s = follow;
                            AppendCodePoint(out, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
                            break;
                        }
                    }
                    AppendCodePoint(out, unit);
                    break;
                }
                default: return false;
            }
            continue;
        }
        std::size_t length = 0;
        unsigned codePoint = 0;
        if (!DecodeUtf8Char(reinterpret_cast<const unsigned char*>(s.text.data()) + s.pos,
                            s.text.size() - s.pos, length, codePoint)) {
            return false;
        }
        s.pos += length;
        AppendCodePoint(out, codePoint);
    }
}

// Scans a JSON number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
bool ScanJsonNumber(Scanner& s, std::string_view& token) {
    const std::size_t begin = s.pos;
    if (s.pos < s.text.size() && s.text[s.pos] == '-') ++s.pos;
    if (s.pos >= s.text.size()) {
        s.pos = begin;
        return false;
    }
    if (s.text[s.pos] == '0') {
        ++s.pos;
    } else if (s.text[s.pos] >= '1' && s.text[s.pos] <= '9') {
        while (s.pos < s.text.size() && s.text[s.pos] >= '0' && s.text[s.pos] <= '9') ++s.pos;
    } else {
        s.pos = begin;
        return false;
    }
    if (s.pos < s.text.size() && s.text[s.pos] == '.') {
        ++s.pos;
        std::size_t digits = 0;
        while (s.pos < s.text.size() && s.text[s.pos] >= '0' && s.text[s.pos] <= '9') {
            ++s.pos;
            ++digits;
        }
        if (digits == 0) {
            s.pos = begin;
            return false;
        }
    }
    if (s.pos < s.text.size() && (s.text[s.pos] == 'e' || s.text[s.pos] == 'E')) {
        ++s.pos;
        if (s.pos < s.text.size() && (s.text[s.pos] == '+' || s.text[s.pos] == '-')) ++s.pos;
        std::size_t digits = 0;
        while (s.pos < s.text.size() && s.text[s.pos] >= '0' && s.text[s.pos] <= '9') {
            ++s.pos;
            ++digits;
        }
        if (digits == 0) {
            s.pos = begin;
            return false;
        }
    }
    token = s.text.substr(begin, s.pos - begin);
    return true;
}

// Locale-independent conversion.  The token already matches the JSON number
// grammar, so only range errors remain possible.
bool ParseNumberValue(const std::string_view& token, double& value) {
    const char* first = token.data();
    const char* last = token.data() + token.size();
    const std::from_chars_result result =
        std::from_chars(first, last, value, std::chars_format::general);
    return result.ec == std::errc() && result.ptr == last;
}

bool ExactInt(double value, int& out) {
    if (!std::isfinite(value) || value != std::floor(value)) return false;
    const double lowest = static_cast<double>((std::numeric_limits<int>::min)());
    const double highest = static_cast<double>((std::numeric_limits<int>::max)());
    if (value < lowest || value > highest) return false;
    out = static_cast<int>(value);
    return true;
}

// Values collected from the document before validation.
struct ParsedFields {
    unsigned seen = 0;
    std::wstring format;
    double schemaVersion = 0.0;
    std::wstring name;
    float erp = 0.f;
    float cfm = 0.f;
    int substeps = 0;
    int rateMultiplier = 0;
    int baseRateHz = 0;
};

bool ParseValue(Scanner& s, Field field, ParsedFields& fields, SettingsError& error) {
    if (field == Field::Format || field == Field::Name) {
        std::wstring text;
        if (!ScanJsonString(s, text)) {
            error = SettingsError::InvalidJson;
            return false;
        }
        if (field == Field::Format) fields.format = std::move(text);
        else fields.name = std::move(text);
        return true;
    }
    if (s.pos >= s.text.size()) {
        error = SettingsError::InvalidJson;
        return false;
    }
    const char first = s.text[s.pos];
    if (first != '-' && (first < '0' || first > '9')) {
        error = SettingsError::InvalidJson;  // wrong value type (string, true, null, ...)
        return false;
    }
    std::string_view token;
    if (!ScanJsonNumber(s, token)) {
        error = SettingsError::InvalidJson;
        return false;
    }
    double value = 0.0;
    if (!ParseNumberValue(token, value)) {
        // JSON-legal token whose magnitude overflows the double range.
        error = FieldError(field);
        return false;
    }
    switch (field) {
        case Field::SchemaVersion:
            fields.schemaVersion = value;
            return true;
        case Field::Erp:
            if (!std::isfinite(value) || value < kErpMin || value > kErpMax) {
                error = SettingsError::InvalidErp;
                return false;
            }
            fields.erp = static_cast<float>(value);
            return true;
        case Field::Cfm:
            if (!std::isfinite(value) || value < kCfmMin || value > kCfmMax) {
                error = SettingsError::InvalidCfm;
                return false;
            }
            fields.cfm = static_cast<float>(value);
            return true;
        case Field::Substeps: {
            int parsed = 0;
            if (!ExactInt(value, parsed) || parsed < kSubstepsMin || parsed > kSubstepsMax) {
                error = SettingsError::InvalidSubsteps;
                return false;
            }
            fields.substeps = parsed;
            return true;
        }
        case Field::RateMultiplier: {
            int parsed = 0;
            if (!ExactInt(value, parsed) || parsed < kMultiplierMin || parsed > kMultiplierMax) {
                error = SettingsError::InvalidMultiplier;
                return false;
            }
            fields.rateMultiplier = parsed;
            return true;
        }
        case Field::BaseRateHz: {
            int parsed = 0;
            if (!ExactInt(value, parsed) || (parsed != kBaseRateA && parsed != kBaseRateB)) {
                error = SettingsError::InvalidBaseRate;
                return false;
            }
            fields.baseRateHz = parsed;
            return true;
        }
        default:
            error = SettingsError::InvalidJson;
            return false;
    }
}

bool ParsePresetObject(Scanner& s, ParsedFields& fields, SettingsError& error) {
    SkipWhitespace(s);
    if (s.pos >= s.text.size() || s.text[s.pos] != '{') {
        error = SettingsError::InvalidJson;
        return false;
    }
    ++s.pos;
    SkipWhitespace(s);
    if (s.pos < s.text.size() && s.text[s.pos] == '}') {
        ++s.pos;
        return true;
    }
    for (;;) {
        SkipWhitespace(s);
        std::wstring key;
        if (!ScanJsonString(s, key)) {
            error = SettingsError::InvalidJson;
            return false;
        }
        const Field field = LookupField(key);
        if (field == Field::Unknown) {
            error = SettingsError::InvalidJson;
            return false;
        }
        const unsigned bit = 1u << static_cast<unsigned>(field);
        if ((fields.seen & bit) != 0) {
            error = SettingsError::InvalidJson;  // duplicate key
            return false;
        }
        fields.seen |= bit;
        SkipWhitespace(s);
        if (s.pos >= s.text.size() || s.text[s.pos] != ':') {
            error = SettingsError::InvalidJson;
            return false;
        }
        ++s.pos;
        SkipWhitespace(s);
        if (!ParseValue(s, field, fields, error)) return false;
        SkipWhitespace(s);
        if (s.pos < s.text.size() && s.text[s.pos] == ',') {
            ++s.pos;
            continue;
        }
        if (s.pos < s.text.size() && s.text[s.pos] == '}') {
            ++s.pos;
            return true;
        }
        error = SettingsError::InvalidJson;
        return false;
    }
}

// ---- number formatting (classic locale, max_digits10) --------------------

std::string FormatFloat(float value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.setf(std::ios::fmtflags(0), std::ios::floatfield);
    stream.precision((std::numeric_limits<float>::max_digits10));
    stream << value;
    return stream.str();
}

std::string FormatInt(int value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << value;
    return stream.str();
}

std::string EncodeJsonString(const std::wstring& value) {
    std::string out;
    out.reserve(value.size() * 3);
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned codePoint = static_cast<unsigned>(value[i]);
        if (sizeof(wchar_t) == 2 && codePoint >= 0xD800u && codePoint <= 0xDBFFu &&
            i + 1 < value.size()) {
            const unsigned low = static_cast<unsigned>(value[i + 1]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                codePoint = 0x10000u + ((codePoint - 0xD800u) << 10) + (low - 0xDC00u);
                ++i;
            }
        }
        if (codePoint == '"') { out += "\\\""; continue; }
        if (codePoint == '\\') { out += "\\\\"; continue; }
        if (codePoint == '\b') { out += "\\b"; continue; }
        if (codePoint == '\f') { out += "\\f"; continue; }
        if (codePoint == '\n') { out += "\\n"; continue; }
        if (codePoint == '\r') { out += "\\r"; continue; }
        if (codePoint == '\t') { out += "\\t"; continue; }
        if (codePoint < 0x20u) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", codePoint);
            out += buffer;
            continue;
        }
        if (codePoint < 0x80u) {
            out.push_back(static_cast<char>(codePoint));
            continue;
        }
        if (codePoint < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
            continue;
        }
        if (codePoint < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
            continue;
        }
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
    return out;
}

// Unique temp name next to the target.  Each attempt differs, so CREATE_NEW
// never overwrites a file that already exists.
std::wstring MakeTempPath(const wchar_t* path, unsigned attempt) {
    std::wstring temp(path);
    temp += L".mmdtmp-";
    temp += std::to_wstring(static_cast<unsigned long long>(GetCurrentProcessId()));
    temp += L'-';
    temp += std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
    temp += L'-';
    temp += std::to_wstring(attempt);
    return temp;
}

}  // namespace

Settings BulletDefaults() {
    // Default-constructed engine values, not values copied from a screenshot
    // or a previously imported preset: btContactSolverInfo's constructor sets
    // m_erp to 0.2 and m_globalCfm to 0.0 (Bullet 2.75, btContactSolverInfo.h).
    const btContactSolverInfo info;
    Settings settings;
    settings.erp = static_cast<float>(info.m_erp);
    settings.cfm = static_cast<float>(info.m_globalCfm);
    settings.substeps = 1;         // explicit time subdivision
    settings.rateMultiplier = 1;
    settings.baseRateHz = 60;
    settings.name = kDefaultName;
    return settings;
}

bool ValidateSettings(const Settings& settings, SettingsError& error) {
    error = SettingsError::None;
    if (!std::isfinite(settings.erp) || settings.erp < 0.f || settings.erp > 1.f) {
        error = SettingsError::InvalidErp;
        return false;
    }
    if (!std::isfinite(settings.cfm) || settings.cfm < 0.f || settings.cfm > 1000000.f) {
        error = SettingsError::InvalidCfm;
        return false;
    }
    if (settings.substeps < kSubstepsMin || settings.substeps > kSubstepsMax) {
        error = SettingsError::InvalidSubsteps;
        return false;
    }
    if (settings.rateMultiplier < kMultiplierMin || settings.rateMultiplier > kMultiplierMax) {
        error = SettingsError::InvalidMultiplier;
        return false;
    }
    if (settings.baseRateHz != kBaseRateA && settings.baseRateHz != kBaseRateB) {
        error = SettingsError::InvalidBaseRate;
        return false;
    }
    if (!NameIsValid(settings.name)) {
        error = SettingsError::InvalidName;
        return false;
    }
    return true;
}

bool DecodePreset(std::string_view json, Settings& settings, SettingsError& error) {
    if (json.size() > kMaxPresetBytes) {
        error = SettingsError::TooLarge;
        return false;
    }
    Scanner scanner{json, 0};
    if (json.size() >= 3 &&
        static_cast<unsigned char>(json[0]) == 0xEFu &&
        static_cast<unsigned char>(json[1]) == 0xBBu &&
        static_cast<unsigned char>(json[2]) == 0xBFu) {
        scanner.pos = 3;
    }

    ParsedFields fields;
    if (!ParsePresetObject(scanner, fields, error)) return false;
    SkipWhitespace(scanner);
    if (scanner.pos != json.size()) {
        error = SettingsError::InvalidJson;  // trailing garbage after the object
        return false;
    }

    for (const Field field : kFieldOrder) {
        if ((fields.seen & (1u << static_cast<unsigned>(field))) == 0) {
            error = SettingsError::MissingField;
            return false;
        }
    }
    if (fields.format != kFormatId && fields.format != kLegacyFormatId) {
        error = SettingsError::InvalidJson;
        return false;
    }
    if (fields.schemaVersion != kSchemaVersion) {
        error = SettingsError::UnsupportedVersion;
        return false;
    }

    Settings decoded;
    decoded.erp = fields.erp;
    decoded.cfm = fields.cfm;
    decoded.substeps = fields.substeps;
    decoded.rateMultiplier = fields.rateMultiplier;
    decoded.baseRateHz = fields.baseRateHz;
    decoded.name = std::move(fields.name);
    if (!ValidateSettings(decoded, error)) return false;

    settings = std::move(decoded);
    error = SettingsError::None;
    return true;
}

std::string EncodePreset(const Settings& settings) {
    SettingsError validation = SettingsError::None;
    if (!ValidateSettings(settings, validation)) return {};

    std::string out;
    out.reserve(256);
    out += "{\n";
    out += "\t\"format\": \"MikuMikuStudioPhysicsPreset\",\n";
    out += "\t\"schemaVersion\": 1,\n";
    out += "\t\"name\": \"";
    out += EncodeJsonString(settings.name);
    out += "\",\n";
    out += "\t\"erp\": ";
    out += FormatFloat(settings.erp);
    out += ",\n";
    out += "\t\"cfm\": ";
    out += FormatFloat(settings.cfm);
    out += ",\n";
    out += "\t\"substeps\": ";
    out += FormatInt(settings.substeps);
    out += ",\n";
    out += "\t\"simulationRateMultiplier\": ";
    out += FormatInt(settings.rateMultiplier);
    out += ",\n";
    out += "\t\"baseSimulationRateHz\": ";
    out += FormatInt(settings.baseRateHz);
    out += "\n}";
    return out;
}

bool LoadPreset(const wchar_t* path, Settings& settings, SettingsError& error) {
    error = SettingsError::IoRead;
    if (path == nullptr || *path == L'\0') return false;

    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == 0 || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    if (static_cast<unsigned long long>(size.QuadPart) > kMaxPresetBytes) {
        CloseHandle(file);
        error = SettingsError::TooLarge;
        return false;
    }

    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    bool read = true;
    if (!text.empty()) {
        DWORD got = 0;
        read = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) != 0 &&
               got == static_cast<DWORD>(text.size());
    }
    CloseHandle(file);
    if (!read) return false;  // error already IoRead

    return DecodePreset(text, settings, error);
}

bool SavePreset(const wchar_t* path, const Settings& settings, SettingsError& error) {
    error = SettingsError::IoWrite;
    if (path == nullptr || *path == L'\0') return false;

    SettingsError validation = SettingsError::None;
    if (!ValidateSettings(settings, validation)) {
        error = validation;
        return false;
    }
    const std::string text = EncodePreset(settings);
    if (text.empty()) return false;  // defensive: validated settings always encode

    // Same-directory unique temp file, created with CREATE_NEW so an
    // existing file (someone else's) is never opened or removed.
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring tempPath;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        tempPath = MakeTempPath(path, attempt);
        file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD reason = GetLastError();
        if (reason != ERROR_FILE_EXISTS && reason != ERROR_ALREADY_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0;
    ok = ok && written == static_cast<DWORD>(text.size());
    ok = ok && FlushFileBuffers(file) != 0;
    if (CloseHandle(file) == 0) ok = false;
    if (!ok) {
        DeleteFileW(tempPath.c_str());  // only the file this call created
        return false;
    }

    if (MoveFileExW(tempPath.c_str(), path,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    error = SettingsError::None;
    return true;
}

}  // namespace mikudancestudio::bake
