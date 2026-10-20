#pragma once
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>

namespace mikumikustudio::nvidia_antialias_fix {
// Private to the optional installer DLL and its driver-contract tests.
struct State {
    bool available = false;
    bool enabled = false;
    std::wstring error;
    int driverStatus = 0;
};
}

namespace mikumikustudio::nvidia_antialias_fix::detail {
// Minimal ABI declarations from NVIDIA's public nvapi.h / nvapi_interface.h.
// Keep V1 structures and pack(4), including the binary union's length word.
using U32 = std::uint32_t;
using Handle = void*;
constexpr U32 kFix = 0x000858f7;
constexpr int kProfileNotFound = -163, kSettingNotFound = -160;
constexpr int kExecutableNotFound = -166;
constexpr int kInsufficientPrivileges = -137;
constexpr unsigned kText = 2048;
#pragma pack(push, 4)
struct Value { U32 value; unsigned char remainder[4096]; };
struct Setting {
    U32 version = sizeof(Setting) | (1u << 16);
    wchar_t name[kText]{};
    U32 id = kFix, type = 0, location = 0, currentPredefined = 0, predefinedValid = 0;
    Value predefined{}, current{};
};
struct Profile {
    U32 version = sizeof(Profile) | (1u << 16);
    wchar_t name[kText]{};
    U32 gpu = 0, predefined = 0, apps = 0, settings = 0;
};
struct Application {
    U32 version = sizeof(Application) | (1u << 16), predefined = 0;
    wchar_t name[kText]{}, friendly[kText]{}, launcher[kText]{};
};
#pragma pack(pop)
static_assert(sizeof(Setting) == 12320 && sizeof(Profile) == 4116 && sizeof(Application) == 12296);

struct Api {
    int (__cdecl* createSession)(Handle*) = nullptr;
    int (__cdecl* destroySession)(Handle) = nullptr;
    int (__cdecl* load)(Handle) = nullptr;
    int (__cdecl* save)(Handle) = nullptr;
    int (__cdecl* findProfile)(Handle, const wchar_t*, Handle*) = nullptr;
    int (__cdecl* createProfile)(Handle, Profile*, Handle*) = nullptr;
    int (__cdecl* profileInfo)(Handle, Handle, Profile*) = nullptr;
    int (__cdecl* enumApps)(Handle, Handle, U32, U32*, Application*) = nullptr;
    int (__cdecl* createApp)(Handle, Handle, Application*) = nullptr;
    int (__cdecl* findApp)(Handle, const wchar_t*, Handle*, Application*) = nullptr;
    int (__cdecl* get)(Handle, Handle, U32, Setting*) = nullptr;
    int (__cdecl* set)(Handle, Handle, Setting*) = nullptr;
    int (__cdecl* remove)(Handle, Handle, U32) = nullptr;
    int (__cdecl* globalProfile)(Handle, Handle*) = nullptr;
    int (__cdecl* baseProfile)(Handle, Handle*) = nullptr;
};
inline std::wstring NormalizePath(std::wstring path) {
    for (auto& c : path) c = c == L'\\' ? L'/' : wchar_t(towlower(c));
    return path;
}
inline std::wstring ProfileName(const std::wstring& path, bool legacy = false) {
    std::uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : NormalizePath(path)) { hash ^= unsigned(c); hash *= 1099511628211ull; }
    wchar_t suffix[32]{};
    swprintf_s(suffix, L"%016llx", static_cast<unsigned long long>(hash));
    return std::wstring(legacy ? L"MikuDanceStudio AA fix " : L"MikuMikuStudio AA fix ") + suffix;
}
inline bool InspectEffective(Api& api, const std::wstring& path, State& state) {
    state = {};
    if (path.size() < 4 || path.size() >= kText || path[1] != L':' ||
        (path[2] != L'\\' && path[2] != L'/')) {
        state.error = L"Invalid executable path"; return false;
    }
    struct Session {
        Api& api; Handle value = nullptr;
        ~Session() { if (value) api.destroySession(value); }
    } session{api};
    auto check = [&](int status, const wchar_t* operation) {
        if (status == 0) return true;
        state.driverStatus = status;
        state.error = std::wstring(operation) + L" (NVAPI " + std::to_wstring(status) + L")";
        return false;
    };
    if (!check(api.createSession(&session.value), L"CreateSession") ||
        !check(api.load(session.value), L"LoadSettings")) return false;
    state.available = true;
    Handle profile = nullptr; Application application;
    int status = api.findApp(session.value, path.c_str(), &profile, &application);
    if (status != kExecutableNotFound && !check(status, L"FindApplicationByName")) return false;
    // GetSetting returns the effective value, including its inheritance location.
    // A valid application override takes precedence over a global/base value.
    auto read = [&](Handle source) {
        Setting setting;
        const int result = api.get(session.value, source, kFix, &setting);
        if (result == 0) state.enabled = setting.type == 0 && setting.current.value == 0;
        return result;
    };
    if (status == 0) {
        status = read(profile);
        if (status == 0) return true;
        if (status != kSettingNotFound) return check(status, L"GetEffectiveSetting");
    }
    if (!check(api.globalProfile(session.value, &profile), L"GetCurrentGlobalProfile")) return false;
    status = read(profile);
    if (status == 0) return true;
    if (status != kSettingNotFound) return check(status, L"GetGlobalSetting");
    if (!check(api.baseProfile(session.value, &profile), L"GetBaseProfile")) return false;
    status = read(profile);
    return status == kSettingNotFound || check(status, L"GetBaseSetting");
}
inline bool Execute(Api& api, const std::wstring& path, const bool* enabled, State& state) {
    state = {};
    if (path.size() < 4 || path.size() >= kText || path.find(L':') != 1 ||
        (path[2] != L'\\' && path[2] != L'/')) {
        state.error = L"Invalid executable path"; return false;
    }
    struct Session {
        Api& api; Handle value = nullptr;
        ~Session() { if (value) api.destroySession(value); }
    } session{api};
    auto check = [&](int status, const wchar_t* operation) {
        if (status == 0) return true;
        state.driverStatus = status;
        state.error = std::wstring(operation) + L" (NVAPI " + std::to_wstring(status) + L")";
        return false;
    };
    if (!check(api.createSession(&session.value), L"CreateSession") ||
        !check(api.load(session.value), L"LoadSettings")) return false;
    state.available = true;
    auto name = ProfileName(path);
    Handle profile = nullptr;
    int status = api.findProfile(session.value, name.c_str(), &profile);
    if (status == kProfileNotFound) {
        // Retain ownership of profiles installed before the host rename. The
        // same exclusive-profile checks below apply to both names.
        const auto legacyName = ProfileName(path, true);
        status = api.findProfile(session.value, legacyName.c_str(), &profile);
        if (status == 0) name = legacyName;
    }
    if (status == kProfileNotFound) {
        if (!enabled || !*enabled) return true;
        Profile info; wcscpy_s(info.name, name.c_str());
        if (!check(api.createProfile(session.value, &info, &profile), L"CreateProfile")) return false;
        Application app; wcscpy_s(app.name, path.c_str());
        wcscpy_s(app.friendly, L"MikuMikuStudio");
        if (!check(api.createApp(session.value, profile, &app), L"CreateApplication")) return false;
    } else if (!check(status, L"FindProfileByName")) return false;
    // Never change a shared, predefined, or mismatched profile, even if someone
    // has renamed it to our expected name. Failed checks discard the session.
    Profile info;
    if (!check(api.profileInfo(session.value, profile, &info), L"GetProfileInfo")) return false;
    Application app; U32 count = 1;
    if (info.predefined || info.apps != 1 || _wcsicmp(info.name, name.c_str()) ||
        !check(api.enumApps(session.value, profile, 0, &count, &app), L"EnumApplications") ||
        count != 1 || app.predefined || NormalizePath(app.name) != NormalizePath(path) || app.launcher[0]) {
        state.error = L"Profile is not exclusive to this executable"; return false;
    }
    Handle applied = nullptr; Application selected;
    if (!check(api.findApp(session.value, path.c_str(), &applied, &selected), L"FindApplicationByName") ||
        applied != profile) {
        state.error = L"Driver selected a different application profile"; return false;
    }
    Setting setting;
    status = api.get(session.value, profile, kFix, &setting);
    if (status != kSettingNotFound && !check(status, L"GetSetting")) return false;
    state.enabled = status == 0 && setting.type == 0 && setting.location == 0 &&
        !setting.currentPredefined && setting.current.value == 0;
    if (!enabled) return true;
    if (*enabled && state.enabled) return true;
    if (*enabled) {
        Setting value; value.current.value = 0; // NVIDIA AA fix: 0 = On, 1 = Off.
        if (!check(api.set(session.value, profile, &value), L"SetSetting")) return false;
    } else if (status == 0 && setting.location == 0 && !setting.currentPredefined) {
        if (!check(api.remove(session.value, profile, kFix), L"DeleteProfileSetting")) return false;
    } else return true;
    if (!check(api.save(session.value), L"SaveSettings")) return false;
    // Read the persisted database, not only the session's staged value.
    if (!check(api.load(session.value), L"ReloadSettings")) return false;
    if (!check(api.findProfile(session.value, name.c_str(), &profile), L"FindSavedProfile")) return false;
    setting = Setting{};
    status = api.get(session.value, profile, kFix, &setting);
    if (status != kSettingNotFound && !check(status, L"ReadBackSetting")) return false;
    state.enabled = status == 0 && setting.type == 0 && setting.location == 0 &&
        !setting.currentPredefined && setting.current.value == 0;
    if (state.enabled != *enabled) { state.error = L"Driver setting readback did not match"; return false; }
    return true;
}
}
