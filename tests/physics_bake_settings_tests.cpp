// MAINTENANCE: regression-keep; physics bake preset validation, JSON decode/encode, file IO.
//
// Console checks for src/physics/physics_bake_settings.cpp.  The source stays
// ASCII-only: Unicode names are written as \u / \x escapes so the expected
// values do not depend on the compiler's source code page.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikudancestudio/physics_bake_settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <locale>
#include <string>
#include <vector>

using namespace mikudancestudio;
namespace bake = mikudancestudio::bake;

namespace {

void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}

bake::Settings Sentinel() {
    bake::Settings s;
    s.erp = 0.75f;
    s.cfm = 12.5f;
    s.substeps = 7;
    s.rateMultiplier = 5;
    s.baseRateHz = 60;
    s.name = L"sentinel name";
    return s;
}

bool Same(const bake::Settings& a, const bake::Settings& b) {
    return a.erp == b.erp && a.cfm == b.cfm && a.substeps == b.substeps &&
           a.rateMultiplier == b.rateMultiplier && a.baseRateHz == b.baseRateHz && a.name == b.name;
}

// Settings matching MakeJson output for any name.
bake::Settings Named(const wchar_t* name) {
    bake::Settings s;
    s.erp = 0.2f;
    s.cfm = 0.f;
    s.substeps = 1;
    s.rateMultiplier = 1;
    s.baseRateHz = 60;
    s.name = name;
    return s;
}

// Complete valid document with the name value supplied as a JSON literal
// (including its quotes).
std::string MakeJson(const std::string& nameLiteral) {
    std::string json = "{\n";
    json += "\"format\": \"MikuMikuStudioPhysicsPreset\",\n";
    json += "\"schemaVersion\": 1,\n";
    json += "\"name\": ";
    json += nameLiteral;
    json += ",\n";
    json += "\"erp\": 0.2,\n\"cfm\": 0.0,\n\"substeps\": 1,\n";
    json += "\"simulationRateMultiplier\": 1,\n\"baseSimulationRateHz\": 60\n}";
    return json;
}

// Replaces one fixture substring; the substring must exist.
std::string With(std::string json, const std::string& from, const std::string& to) {
    const std::size_t at = json.find(from);
    Check(at != std::string::npos, "test fixture substring is present");
    return json.replace(at, from.size(), to);
}

void ExpectOk(const std::string& json, const bake::Settings& expected, const char* what) {
    bake::Settings target = Sentinel();
    bake::SettingsError error = bake::SettingsError::IoRead;
    Check(bake::DecodePreset(json, target, error), what);
    Check(error == bake::SettingsError::None, what);
    Check(Same(target, expected), what);
}

void ExpectError(const std::string& json, bake::SettingsError expected, const char* what) {
    bake::Settings keep = Sentinel();
    bake::Settings target = keep;
    bake::SettingsError error = bake::SettingsError::None;
    Check(!bake::DecodePreset(json, target, error), what);
    Check(error == expected, what);
    Check(Same(target, keep), "failed decode leaves the current settings untouched");
}

void ExpectInvalid(const bake::Settings& settings, bake::SettingsError expected, const char* what) {
    bake::SettingsError error = bake::SettingsError::None;
    Check(!bake::ValidateSettings(settings, error), what);
    Check(error == expected, what);
}

// ---- defaults ------------------------------------------------------------

void TestDefaults() {
    const bake::Settings d = bake::BulletDefaults();
    Check(d.erp == 0.2f, "Bullet defaults take ERP from btContactSolverInfo (0.2)");
    Check(d.cfm == 0.f, "Bullet defaults take CFM from btContactSolverInfo (0.0)");
    Check(d.substeps == 1, "Bullet defaults use one explicit substep");
    Check(d.rateMultiplier == 1, "Bullet defaults use rate multiplier 1");
    Check(d.baseRateHz == 60, "Bullet defaults use a 60 Hz base rate");
    Check(d.name == L"Bullet defaults", "Bullet defaults are named 'Bullet defaults'");
    bake::SettingsError error = bake::SettingsError::InvalidJson;
    Check(bake::ValidateSettings(d, error), "Bullet defaults validate");
    Check(error == bake::SettingsError::None, "successful validation reports no error");
}

// ---- validation ----------------------------------------------------------

void TestValidation() {
    bake::SettingsError error = bake::SettingsError::None;
    bake::Settings s = Named(L"ok");
    s.cfm = 0.1f;
    Check(bake::ValidateSettings(s, error), "valid settings pass");

    s = Named(L"x");
    s.cfm = 1000000.f;
    s.substeps = 16;
    s.rateMultiplier = 16;
    s.baseRateHz = 30;
    Check(bake::ValidateSettings(s, error), "value boundaries are accepted");

    s = Named(L"x");
    s.erp = -0.1f;
    ExpectInvalid(s, bake::SettingsError::InvalidErp, "negative ERP rejected");
    s.erp = 1.1f;
    ExpectInvalid(s, bake::SettingsError::InvalidErp, "ERP above 1 rejected");
    s.erp = (std::numeric_limits<float>::quiet_NaN)();
    ExpectInvalid(s, bake::SettingsError::InvalidErp, "NaN ERP rejected");
    s.erp = (std::numeric_limits<float>::infinity)();
    ExpectInvalid(s, bake::SettingsError::InvalidErp, "infinite ERP rejected");

    s = Named(L"x");
    s.cfm = -1.f;
    ExpectInvalid(s, bake::SettingsError::InvalidCfm, "negative CFM rejected");
    s.cfm = 1000001.f;
    ExpectInvalid(s, bake::SettingsError::InvalidCfm, "CFM above 1e6 rejected");
    s.cfm = (std::numeric_limits<float>::quiet_NaN)();
    ExpectInvalid(s, bake::SettingsError::InvalidCfm, "NaN CFM rejected");

    s = Named(L"x");
    s.substeps = 0;
    ExpectInvalid(s, bake::SettingsError::InvalidSubsteps, "zero substeps rejected");
    s.substeps = 17;
    ExpectInvalid(s, bake::SettingsError::InvalidSubsteps, "substeps above 16 rejected");

    s = Named(L"x");
    s.rateMultiplier = 0;
    ExpectInvalid(s, bake::SettingsError::InvalidMultiplier, "zero rate multiplier rejected");
    s.rateMultiplier = 17;
    ExpectInvalid(s, bake::SettingsError::InvalidMultiplier, "rate multiplier above 16 rejected");

    s = Named(L"x");
    s.baseRateHz = 31;
    ExpectInvalid(s, bake::SettingsError::InvalidBaseRate, "base rate other than 30/60 rejected");
    s.baseRateHz = 120;
    ExpectInvalid(s, bake::SettingsError::InvalidBaseRate, "base rate 120 rejected");

    s = Named(L"x");
    s.name = std::wstring(128, L'a');
    Check(bake::ValidateSettings(s, error), "128 code point name accepted");
    s.name = std::wstring(129, L'a');
    ExpectInvalid(s, bake::SettingsError::InvalidName, "129 code point name rejected");

    std::wstring astral;
    for (int i = 0; i < 128; ++i) astral += L"\U0001F600";
    s.name = astral;
    Check(bake::ValidateSettings(s, error), "128 astral code points count as 128 characters");
    astral += L"\U0001F600";
    s.name = astral;
    ExpectInvalid(s, bake::SettingsError::InvalidName, "129 astral code points rejected");

    std::wstring control = L"ab";
    control.push_back(wchar_t(7));
    control += L"cd";
    s.name = control;
    ExpectInvalid(s, bake::SettingsError::InvalidName, "control character in name rejected");
    std::wstring del = L"ab";
    del.push_back(wchar_t(0x7F));
    s.name = del;
    ExpectInvalid(s, bake::SettingsError::InvalidName, "DEL character in name rejected");

    std::wstring loneHigh = L"ab";
    loneHigh.push_back(wchar_t(0xD83D));
    s.name = loneHigh;
    ExpectInvalid(s, bake::SettingsError::InvalidName, "lone high surrogate in name rejected");
    std::wstring loneLow = L"ab";
    loneLow.push_back(wchar_t(0xDEAD));
    s.name = loneLow;
    ExpectInvalid(s, bake::SettingsError::InvalidName, "lone low surrogate in name rejected");
}

// ---- encode / decode round trip -----------------------------------------

void TestRoundTrip() {
    bake::Settings s;
    s.erp = 0.2f;
    s.cfm = 0.1f;
    s.substeps = 3;
    s.rateMultiplier = 4;
    s.baseRateHz = 30;
    s.name = L"\u622A\u56FE\u53C2\u6570\u793A\u4F8B \U0001F600 \"quoted\" \\ back";

    const std::string encoded = bake::EncodePreset(s);
    Check(!encoded.empty(), "encode produces text for valid settings");
    Check(encoded.find("\"format\": \"MikuMikuStudioPhysicsPreset\"") != std::string::npos,
          "encode writes the format identifier");
    Check(encoded.find("\"schemaVersion\": 1") != std::string::npos,
          "encode writes schemaVersion 1");
    Check(encoded.find("0.200000003") != std::string::npos,
          "encode writes floats with max_digits10 so they round-trip");

    bake::Settings decoded = Sentinel();
    bake::SettingsError error = bake::SettingsError::IoRead;
    Check(bake::DecodePreset(encoded, decoded, error), "encoded preset decodes again");
    Check(error == bake::SettingsError::None, "successful decode reports no error");
    Check(Same(decoded, s), "encode/decode round trip preserves every field exactly");

    bake::Settings invalid = s;
    invalid.baseRateHz = 45;
    Check(bake::EncodePreset(invalid).empty(), "encode refuses a bad base rate");
    invalid = s;
    invalid.name = L"bad\u0007name";
    Check(bake::EncodePreset(invalid).empty(), "encode refuses a control character in the name");
    invalid = s;
    invalid.cfm = (std::numeric_limits<float>::quiet_NaN)();
    Check(bake::EncodePreset(invalid).empty(), "encode refuses NaN");
}

// ---- decode: accepted inputs --------------------------------------------

void TestDecodeAccepts() {
    ExpectOk(MakeJson("\"plain\""), Named(L"plain"), "plain ASCII name decodes");
    ExpectOk(MakeJson("\"A\\u0042\\u622A\\u56FE\\uD83D\\uDE00\""),
             Named(L"AB\u622A\u56FE\U0001F600"),
             "\\uXXXX escapes with a surrogate pair decode");
    ExpectOk(MakeJson("\"\xe6\x88\xaa\xe5\x9b\xbe\""), Named(L"\u622A\u56FE"),
             "raw UTF-8 name bytes decode");
    ExpectOk(std::string("\xEF\xBB\xBF") + MakeJson("\"bom\""), Named(L"bom"),
             "leading UTF-8 BOM is accepted");
    ExpectOk(MakeJson("\"quote\\\"slash\\\\\""), Named(L"quote\"slash\\"),
             "quote and backslash escapes round-trip in the name");

    const std::string sci =
        "{\"baseSimulationRateHz\": 3e1, \"simulationRateMultiplier\": 2.0, \"substeps\": 4e0,"
        " \"cfm\": 1.25e1, \"erp\": 2e-1, \"name\": \"sci\","
        " \"format\": \"MikuMikuStudioPhysicsPreset\", \"schemaVersion\": 1e0}";
    bake::Settings expected;
    expected.erp = 0.2f;
    expected.cfm = 12.5f;
    expected.substeps = 4;
    expected.rateMultiplier = 2;
    expected.baseRateHz = 30;
    expected.name = L"sci";
    ExpectOk(sci, expected, "scientific notation and integer-valued decimals decode");

    const std::string spaced =
        "\t { \n\"name\" : \"spaced\" ,\r\n \"schemaVersion\" : 1 , "
        "\"format\": \"MikuMikuStudioPhysicsPreset\" , \"cfm\" : 0.5 , \"erp\" : 1 , "
        "\"baseSimulationRateHz\" : 60 , \"simulationRateMultiplier\" : 1 , \"substeps\" : 16 \n}\n\n";
    expected.erp = 1.f;
    expected.cfm = 0.5f;
    expected.substeps = 16;
    expected.rateMultiplier = 1;
    expected.baseRateHz = 60;
    expected.name = L"spaced";
    ExpectOk(spaced, expected, "whitespace, key order and value boundaries decode");
}

// ---- decode: rejected inputs --------------------------------------------

void TestDecodeRejects() {
    ExpectError("", bake::SettingsError::InvalidJson, "empty input is rejected");
    ExpectError("[]", bake::SettingsError::InvalidJson, "a non-object document is rejected");
    ExpectError("null", bake::SettingsError::InvalidJson, "a bare literal is rejected");
    ExpectError("{", bake::SettingsError::InvalidJson, "a truncated object is rejected");
    ExpectError(MakeJson("\"x\"").substr(0, 20), bake::SettingsError::InvalidJson,
                "a truncated document is rejected");
    ExpectError("{\"format\": }", bake::SettingsError::InvalidJson, "a missing value is rejected");
    ExpectError(MakeJson("\"x\"") + " garbage", bake::SettingsError::InvalidJson,
                "trailing garbage after the object is rejected");
    ExpectError(MakeJson("\"x\"") + "{}", bake::SettingsError::InvalidJson,
                "a second object after the first is rejected");

    ExpectError("{\n\"schemaVersion\": 1\n}", bake::SettingsError::MissingField,
                "missing keys are reported as MissingField");
    ExpectError("{\"format\": \"MikuMikuStudioPhysicsPreset\"}", bake::SettingsError::MissingField,
                "MissingField comes before value checks");

    ExpectError(
        "{\"format\": \"MikuMikuStudioPhysicsPreset\", \"schemaVersion\": 1, \"name\": \"x\","
        " \"erp\": 0.2, \"erp\": 0.3, \"cfm\": 0.0, \"substeps\": 1,"
        " \"simulationRateMultiplier\": 1, \"baseSimulationRateHz\": 60}",
        bake::SettingsError::InvalidJson, "duplicate keys are rejected");
    ExpectError(
        "{\"format\": \"MikuMikuStudioPhysicsPreset\", \"schemaVersion\": 1, \"name\": \"x\","
        " \"extra\": 1, \"erp\": 0.2, \"cfm\": 0.0, \"substeps\": 1,"
        " \"simulationRateMultiplier\": 1, \"baseSimulationRateHz\": 60}",
        bake::SettingsError::InvalidJson, "unknown keys are rejected");
    ExpectError(
        "{\"format\": \"MikuMikuStudioPhysicsPreset\", \"schemaVersion\": 1, \"name\": \"x\","
        " \"erp\": \"0.2\", \"cfm\": 0.0, \"substeps\": 1,"
        " \"simulationRateMultiplier\": 1, \"baseSimulationRateHz\": 60}",
        bake::SettingsError::InvalidJson, "a string where a number belongs is rejected");
    ExpectError(MakeJson("1"), bake::SettingsError::InvalidJson,
                "a number where the name string belongs is rejected");
    ExpectError(MakeJson("true"), bake::SettingsError::InvalidJson,
                "a boolean value is rejected");
    ExpectError(
        "{\"format\": \"MikuMikuStudioPhysicsPreset\", \"schemaVersion\": 1, \"name\": \"x\","
        " \"erp\": 0.2, \"cfm\": 0.0, \"substeps\": 1,"
        " \"simulationRateMultiplier\": 1, \"baseSimulationRateHz\": 60, }",
        bake::SettingsError::InvalidJson, "a trailing comma is rejected");

    const std::string base = MakeJson("\"x\"");
    ExpectError(With(base, "\"schemaVersion\": 1", "\"schemaVersion\": 2"),
                bake::SettingsError::UnsupportedVersion, "schemaVersion 2 is unsupported");
    ExpectError(With(base, "\"schemaVersion\": 1", "\"schemaVersion\": 1.5"),
                bake::SettingsError::UnsupportedVersion,
                "a non-integer schemaVersion is unsupported");
    ExpectError(With(base, "\"schemaVersion\": 1", "\"schemaVersion\": \"1\""),
                bake::SettingsError::InvalidJson, "schemaVersion as a string is rejected");
    ExpectError(With(base, "\"format\": \"MikuMikuStudioPhysicsPreset\"",
                     "\"format\": \"SomethingElse\""),
                bake::SettingsError::InvalidJson,
                "a different format identifier is rejected");

    ExpectError(With(base, "\"erp\": 0.2", "\"erp\": 1e400"), bake::SettingsError::InvalidErp,
                "an overflowing ERP is rejected");
    ExpectError(With(base, "\"erp\": 0.2", "\"erp\": 1.5"), bake::SettingsError::InvalidErp,
                "an out-of-range ERP is rejected");
    ExpectError(With(base, "\"erp\": 0.2", "\"erp\": -0.2"), bake::SettingsError::InvalidErp,
                "a negative ERP is rejected");
    ExpectError(With(base, "\"cfm\": 0.0", "\"cfm\": 2000000"), bake::SettingsError::InvalidCfm,
                "an out-of-range CFM is rejected");
    ExpectError(With(base, "\"cfm\": 0.0", "\"cfm\": NaN"), bake::SettingsError::InvalidJson,
                "a NaN literal is rejected");
    ExpectError(With(base, "\"cfm\": 0.0", "\"cfm\": Infinity"), bake::SettingsError::InvalidJson,
                "an Infinity literal is rejected");
    ExpectError(With(base, "\"substeps\": 1", "\"substeps\": 2.5"),
                bake::SettingsError::InvalidSubsteps, "fractional substeps are rejected");
    ExpectError(With(base, "\"substeps\": 1", "\"substeps\": 17"),
                bake::SettingsError::InvalidSubsteps, "substeps above 16 are rejected");
    ExpectError(With(base, "\"substeps\": 1", "\"substeps\": 1e400"),
                bake::SettingsError::InvalidSubsteps, "an overflowing substeps value is rejected");
    ExpectError(With(base, "\"simulationRateMultiplier\": 1", "\"simulationRateMultiplier\": 0"),
                bake::SettingsError::InvalidMultiplier, "a zero rate multiplier is rejected");
    ExpectError(With(base, "\"baseSimulationRateHz\": 60", "\"baseSimulationRateHz\": 45"),
                bake::SettingsError::InvalidBaseRate, "an unsupported base rate is rejected");
    ExpectError(With(base, "\"erp\": 0.2", "\"erp\": 01"), bake::SettingsError::InvalidJson,
                "a number with a leading zero is rejected");
    ExpectError(With(base, "\"erp\": 0.2", "\"erp\": 0."), bake::SettingsError::InvalidJson,
                "a malformed number is rejected");
    ExpectError(With(base, "\"erp\": 0.2", "\"erp\": 1e"), bake::SettingsError::InvalidJson,
                "a dangling exponent is rejected");

    ExpectError(MakeJson("\"a\\nb\""), bake::SettingsError::InvalidName,
                "an escaped control character in the name is rejected");
    ExpectError(MakeJson("\"tab\\there\""), bake::SettingsError::InvalidName,
                "an escaped tab is a control character and is rejected");
    ExpectError(MakeJson("\"x\\uDEAD\""), bake::SettingsError::InvalidName,
                "a lone low surrogate escape is rejected");
    ExpectError(MakeJson("\"x\\uD83D\""), bake::SettingsError::InvalidName,
                "a lone high surrogate escape is rejected");
    ExpectError(MakeJson("\"a\\qb\""), bake::SettingsError::InvalidJson,
                "an unknown escape is rejected");
    ExpectError(MakeJson("\"a\\u12G4\""), bake::SettingsError::InvalidJson,
                "a malformed \\u escape is rejected");
    ExpectError(MakeJson("\"a\\u12\""), bake::SettingsError::InvalidJson,
                "a truncated \\u escape is rejected");
    ExpectError(MakeJson("\"unterminated"), bake::SettingsError::InvalidJson,
                "an unterminated string is rejected");
    ExpectError(MakeJson(std::string("\"a") + '\x01' + "b\""), bake::SettingsError::InvalidJson,
                "a raw control character inside a string is rejected");

    ExpectError(MakeJson(std::string("\"a") + "\xff" + "b\""), bake::SettingsError::InvalidJson,
                "an invalid UTF-8 lead byte is rejected");
    ExpectError(MakeJson(std::string("\"a") + "\xc0\xaf" + "b\""), bake::SettingsError::InvalidJson,
                "an overlong UTF-8 sequence is rejected");
    ExpectError(MakeJson(std::string("\"a") + "\xed\xa0\x80" + "b\""),
                bake::SettingsError::InvalidJson,
                "a UTF-8 encoded surrogate code point is rejected");
    ExpectError(MakeJson(std::string("\"a") + "\xe4\xb8" + "\""), bake::SettingsError::InvalidJson,
                "a truncated UTF-8 sequence is rejected");

    std::string tooLong = "\"";
    tooLong.append(129, 'a');
    tooLong += "\"";
    ExpectError(MakeJson(tooLong), bake::SettingsError::InvalidName,
                "a 129 character name is rejected");
    std::string justFits = "\"";
    justFits.append(128, 'a');
    justFits += "\"";
    bake::Settings longName = Named(L"x");
    longName.name = std::wstring(128, L'a');
    ExpectOk(MakeJson(justFits), longName, "a 128 character name is accepted");
}

// ---- locale independence -------------------------------------------------

void TestLocaleInvariance() {
    bake::Settings s;
    s.erp = 0.2f;
    s.cfm = 0.1f;
    s.substeps = 3;
    s.rateMultiplier = 2;
    s.baseRateHz = 30;
    s.name = L"locale \u6D4B\u8BD5";
    const std::string before = bake::EncodePreset(s);
    const std::locale previous = std::locale::global(std::locale::classic());
    try {
        std::locale::global(std::locale("de-DE"));
    } catch (...) {
        std::locale::global(std::locale::classic());
    }
    const std::string after = bake::EncodePreset(s);
    Check(before == after, "encode output does not depend on the global locale");
    bake::Settings decoded = Sentinel();
    bake::SettingsError error = bake::SettingsError::IoRead;
    Check(bake::DecodePreset(after, decoded, error) && Same(decoded, s),
          "decode does not depend on the global locale");
    std::locale::global(previous);
}

// ---- file IO -------------------------------------------------------------

std::wstring TestDirectory() {
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    Check(length > 0 && length <= MAX_PATH, "GetTempPathW returns a path");
    std::wstring dir(buffer, length);
    dir += L"mikudance_physics_bake_settings_tests_";
    dir += std::to_wstring(static_cast<unsigned long long>(GetCurrentProcessId()));
    dir += L"_";
    dir += std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
    dir += L"\\";
    Check(CreateDirectoryW(dir.c_str(), nullptr) != 0, "create the test scratch directory");
    return dir;
}

void WriteBytes(const std::wstring& path, const std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create a fixture file");
    DWORD written = 0;
    Check(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
              written == static_cast<DWORD>(bytes.size()),
          "write a fixture file");
    CloseHandle(file);
}

std::string ReadBytes(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open a fixture file for reading");
    std::string bytes;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) != 0 && got > 0) {
        bytes.append(buffer, got);
    }
    CloseHandle(file);
    return bytes;
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<std::wstring> ListFileNames(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    const HANDLE scan = FindFirstFileW((dir + L"*").c_str(), &data);
    if (scan == INVALID_HANDLE_VALUE) return names;
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") continue;
        names.push_back(name);
    } while (FindNextFileW(scan, &data) != 0);
    FindClose(scan);
    return names;
}

void TestFileIo() {
    const std::wstring dir = TestDirectory();
    const std::wstring path = dir + L"preset.json";

    bake::Settings saved;
    saved.erp = 0.35f;
    saved.cfm = 2.5f;
    saved.substeps = 4;
    saved.rateMultiplier = 2;
    saved.baseRateHz = 30;
    saved.name = L"io preset \u622A\u56FE";

    bake::SettingsError error = bake::SettingsError::None;
    Check(bake::SavePreset(path.c_str(), saved, error), "SavePreset writes a valid preset");
    Check(error == bake::SettingsError::None, "a successful save reports no error");
    Check(FileExists(path), "the saved preset exists");

    bake::Settings loaded = Sentinel();
    Check(bake::LoadPreset(path.c_str(), loaded, error), "LoadPreset reads the saved preset");
    Check(error == bake::SettingsError::None, "a successful load reports no error");
    Check(Same(loaded, saved), "save/load round trip preserves every field");

    bake::Settings keep = Sentinel();
    Check(!bake::LoadPreset((dir + L"missing.json").c_str(), keep, error),
          "loading a missing file fails");
    Check(error == bake::SettingsError::IoRead, "a missing file reports IoRead");
    Check(Same(keep, Sentinel()), "a failed load leaves the settings untouched");
    Check(!bake::LoadPreset(nullptr, keep, error) && error == bake::SettingsError::IoRead,
          "a null load path reports IoRead");
    Check(!bake::SavePreset(nullptr, saved, error) && error == bake::SettingsError::IoWrite,
          "a null save path reports IoWrite");

    bake::Settings invalid = saved;
    invalid.substeps = 0;
    const std::string before = ReadBytes(path);
    Check(!bake::SavePreset(path.c_str(), invalid, error), "invalid settings are not saved");
    Check(error == bake::SettingsError::InvalidSubsteps, "invalid settings report their own error");
    Check(ReadBytes(path) == before, "a failed save keeps the previous file content");

    // Foreign files that merely look like our temp files must survive, and a
    // successful save must leave no temp file behind.
    const std::vector<std::wstring> decoys{L"preset.json.mmdtmp-0-0-0", L"preset.json.mmdtmp-7-7-7",
                                           L"preset.json.mmdtmp-decoy"};
    for (const std::wstring& name : decoys) WriteBytes(dir + name, "decoy");
    Check(bake::SavePreset(path.c_str(), saved, error), "saving succeeds with decoy temp names");
    for (const std::wstring& name : decoys) {
        Check(FileExists(dir + name), "foreign temp-looking files are never deleted");
    }
    for (const std::wstring& name : ListFileNames(dir)) {
        if (name == L"preset.json") continue;
        bool known = false;
        for (const std::wstring& decoy : decoys) known = known || name == decoy;
        Check(known, "a successful save leaves no temp file behind");
    }
    loaded = Sentinel();
    Check(bake::LoadPreset(path.c_str(), loaded, error) && Same(loaded, saved),
          "the preset still loads after the decoy save");

    // A target directory that does not exist fails without creating anything.
    const std::vector<std::wstring> namesBefore = ListFileNames(dir);
    Check(!bake::SavePreset((dir + L"no_such_dir\\preset.json").c_str(), saved, error),
          "saving into a missing directory fails");
    Check(error == bake::SettingsError::IoWrite, "a missing target directory reports IoWrite");
    Check(ListFileNames(dir) == namesBefore, "a failed save creates no file at all");

    // Read-only target: reading works, overwriting fails and keeps the file.
    const std::wstring readOnly = dir + L"readonly.json";
    const std::string readOnlyBytes = bake::EncodePreset(saved);
    WriteBytes(readOnly, readOnlyBytes);
    Check(SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_READONLY) != 0,
          "mark the fixture read-only");
    const std::vector<std::wstring> namesWithReadOnly = ListFileNames(dir);
    bake::Settings fromReadOnly = Sentinel();
    Check(bake::LoadPreset(readOnly.c_str(), fromReadOnly, error), "a read-only preset loads");
    Check(Same(fromReadOnly, saved), "the read-only preset loads its values");
    bake::Settings other = saved;
    other.name = L"replacement";
    Check(!bake::SavePreset(readOnly.c_str(), other, error), "overwriting a read-only file fails");
    Check(error == bake::SettingsError::IoWrite, "a read-only target reports IoWrite");
    Check(ReadBytes(readOnly) == readOnlyBytes, "the read-only file keeps its content");
    Check(FileExists(readOnly), "the read-only file is not deleted");
    Check(ListFileNames(dir) == namesWithReadOnly, "a failed save leaves no temp file behind");
    SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_NORMAL);

    // Corrupt bytes and oversized files keep the settings untouched.
    const std::wstring corrupt = dir + L"corrupt.json";
    WriteBytes(corrupt, std::string("{\xff\xfe broken"));
    bake::Settings keepCorrupt = Sentinel();
    Check(!bake::LoadPreset(corrupt.c_str(), keepCorrupt, error), "corrupt bytes fail to load");
    Check(error == bake::SettingsError::InvalidJson, "corrupt bytes report InvalidJson");
    Check(Same(keepCorrupt, Sentinel()), "a corrupt file leaves the settings untouched");

    const std::wstring big = dir + L"big.json";
    std::string bigBytes(70000, ' ');
    bigBytes += bake::EncodePreset(saved);
    WriteBytes(big, bigBytes);
    bake::Settings keepBig = Sentinel();
    Check(!bake::LoadPreset(big.c_str(), keepBig, error), "an oversized preset fails to load");
    Check(error == bake::SettingsError::TooLarge, "an oversized preset reports TooLarge");
    Check(Same(keepBig, Sentinel()), "an oversized file leaves the settings untouched");

    ExpectError(std::string(70000, ' ') + MakeJson("\"x\""), bake::SettingsError::TooLarge,
                "decode rejects input over 64 KiB");

    for (const std::wstring& name : ListFileNames(dir)) DeleteFileW((dir + name).c_str());
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

int wmain(int, wchar_t**) {
    TestDefaults();
    TestValidation();
    TestRoundTrip();
    TestDecodeAccepts();
    TestDecodeRejects();
    TestLocaleInvariance();
    TestFileIo();
    std::printf("physics_bake_settings_tests: all checks passed\n");
    return 0;
}
