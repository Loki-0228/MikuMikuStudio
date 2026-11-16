#include "legacy_discovery.hpp"
#include "loader_win32.hpp"
#include "original_camera_memory.hpp"
#include <fstream>

namespace mikumikustudio::plugins {
namespace {
constexpr auto kMme = "74b3f5882a3bad28131cbc57994e2f960938e6ca850fc7139018ce4822bd87dc";
constexpr auto kEffekseer = "2a7cf3d9afce099228168a1529589f48b5a232befa24cbaa4ab363756c11f532";
constexpr auto kUtility = "c778de083c77a6850b56f98ac8f056d0e8c104c833f24aea420d4715a87612c5";
constexpr size_t kCacheLimit = 16 * 1024 * 1024;
bool Plain(const fs::path& path) {
    auto a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}
void CheckParents(fs::path path) {
    for (;;) {
        if (!Plain(path)) throw std::runtime_error("cache path is missing or a reparse point");
        const auto parent = path.parent_path();
        if (parent == path || parent.empty()) break;
        path = parent;
    }
}
struct File {
    HANDLE handle;
    explicit File(const fs::path& path) : handle(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot inspect plugin file");
    }
    ~File() { CloseHandle(handle); }
    std::string Stamp() const {
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandle(handle, &info) ||
                !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            throw std::runtime_error("cannot identify ordinary plugin file");
        std::string value;
        for (auto n : {uint64_t(info.dwVolumeSerialNumber), uint64_t(info.nFileIndexHigh), uint64_t(info.nFileIndexLow),
                uint64_t(info.nFileSizeHigh), uint64_t(info.nFileSizeLow), uint64_t(basic.LastWriteTime.QuadPart),
                uint64_t(basic.ChangeTime.QuadPart)}) value += std::to_string(n) + ":";
        return value;
    }
};
Manifest Automatic(const fs::path& file, const std::string& hash, const std::string& profile) {
    Manifest m;
    m.file = m.entry = file; m.package = file.parent_path(); m.legacyEntrySha256 = hash;
    m.kind = Manifest::Kind::LegacyMmdPlugin; m.minWindows = {6, 1, 0}; m.legacyProfile = profile;
    if (profile == "mme037" && hash == kMme) {
        m.kind = Manifest::Kind::LegacyMme; m.major = 0; m.legacyProfile.clear();
        m.id = "org.mikumikustudio.legacy.mme"; m.name = "MME 0.37"; m.version = {0, 37, 0};
    } else if (profile == "effekseer_v2" && hash == kEffekseer) {
        m.id = "org.mikumikustudio.legacy.effekseer"; m.name = "EffekseerForMMD"; m.major = 2; m.version = {0, 25, 0};
    } else if (profile == "window_observer_v3" && hash == kUtility) {
        m.id = "org.mikumikustudio.legacy.mmdutility"; m.name = "MMDUtility"; m.major = 3; m.version = {1, 0, 0};
    } else if (profile == "original_camera_926" && hash == kOriginalFirstPersonCameraSha256) {
        m.id = "org.mikumikustudio.legacy.original-first-person-camera"; m.name = "FirstPersonCamera v0.01 (original DLL)";
        m.major = 4; m.version = {0, 1, 0}; m.legacyProfile = "first_person_camera_v4";
    } else if (profile == "first_person_camera_v4") {
        m.id = "org.mikumikustudio.legacy.first-person-camera"; m.name = "FirstPersonCamera (MMS adapter)";
        m.optIn = true;
        m.major = 4; m.version = {0, 1, 0};
    } else throw std::runtime_error("unrecognized cached legacy profile");
    return m;
}
}
std::string JsonQuote(std::string_view input) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : input) {
        if (c == '\\' || c == '"') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}
LegacyDiscovery::LegacyDiscovery(const fs::path& root, Discovery& result, bool useCache)
    : root_(root), result_(result) {
    if (!useCache) return;
    cache_ = root.parent_path() / L"cache/plugins/legacy-discovery.json";
    try {
        if (!fs::exists(cache_)) return;
        PackageFile(cache_.parent_path(), "legacy-discovery.json");
        const auto json = Json::Parse(ReadFile(cache_, kCacheLimit), kCacheLimit);
        if (json.At("schemaVersion").UInt() != 2 || json.At("root").String() != Utf8(Lower(root.native()))) return;
        for (const auto& item : json.At("files").Items()) {
            if (previous_.size() >= 32768) throw std::runtime_error("discovery cache limit");
            previous_.emplace(item.At("path").String(), Entry{item.At("stamp").String(), item.At("hash").String(),
                item.At("profile").String(), item.At("error").String()});
        }
    } catch (const std::exception&) {
        previous_.clear(); result_.diagnostics.push_back("legacy discovery cache ignored; rebuilding");
    }
}
void LegacyDiscovery::Inspect(const fs::path& file) {
    if (current_.size() >= 32768) throw std::runtime_error("discovery DLL limit");
    const auto key = Utf8(Lower(file.lexically_relative(root_).native()));
    try {
        PackageFile(root_, Utf8(file.lexically_relative(root_).native()));
        File locked(file);
        Entry entry; entry.stamp = locked.Stamp();
        const auto old = previous_.find(key);
        if (old != previous_.end() && old->second.stamp == entry.stamp) {
            entry = old->second; ++result_.cachedDlls;
        } else {
            ++result_.inspectedDlls;
            try {
                const auto exports = InspectLegacyExports(file);
                // Hash only plausible plugins. Ordinary dependencies are also cached.
                if (exports.count("version") || (exports.count("Initialize") && exports.count("OnLostDevice") && exports.count("OnResetDevice"))) {
                    entry.hash = Sha256(file);
                    if (entry.hash == kMme) entry.profile = "mme037";
                    else if (entry.hash == kEffekseer) entry.profile = "effekseer_v2";
                    else if (entry.hash == kOriginalFirstPersonCameraSha256) entry.profile = "original_camera_926";
                    else if (entry.hash == kUtility) entry.profile = "window_observer_v3";
                    else if (exports.count("MMSLegacyCameraAdapterVersion") && exports.count("create4") && exports.count("destroy4"))
                        entry.profile = "first_person_camera_v4";
                    else if (exports.count("create4"))
                        entry.error = "unsupported ABI 4 plugin: this original DLL has no verified memory layout/callback profile";
                    else if (exports.count("create2") || exports.count("create3"))
                        entry.error = "unsupported legacy plugin: ABI version alone does not establish callback compatibility";
                    else if (Lower(file.filename().native()) == L"mmeffect.dll")
                        entry.error = "unsupported MME build; automatic discovery supports MME 0.37 x64";
                }
            } catch (const std::exception& e) { entry.error = std::string("DLL inspection failed: ") + e.what(); }
        }
        current_.emplace(key, entry);
        if (!entry.error.empty()) {
            const auto error = Utf8(file.native()) + ": " + entry.error;
            result_.diagnostics.push_back(error); result_.errors.push_back(error);
        }
        if (!entry.profile.empty()) result_.packages.push_back(Automatic(file, entry.hash, entry.profile));
    } catch (const std::exception& e) { const auto error = Utf8(file.native()) + ": " + e.what(); result_.diagnostics.push_back(error); result_.errors.push_back(error); }
}
void LegacyDiscovery::Save() noexcept {
    if (cache_.empty()) return;
    try {
        auto parent = root_.parent_path(); CheckParents(parent);
        for (const auto* part : {L"cache", L"plugins"}) {
            parent /= part; fs::create_directory(parent); CheckParents(parent);
        }
        if (fs::exists(cache_)) PackageFile(parent, "legacy-discovery.json");
        std::string json = "{\"schemaVersion\":2,\"root\":" + JsonQuote(Utf8(Lower(root_.native()))) + ",\"files\":[";
        bool comma = false;
        for (const auto& [path, entry] : current_) {
            if (comma) json += ','; comma = true;
            json += "{\"path\":" + JsonQuote(path) + ",\"stamp\":" + JsonQuote(entry.stamp) +
                ",\"hash\":" + JsonQuote(entry.hash) + ",\"profile\":" + JsonQuote(entry.profile) +
                ",\"error\":" + JsonQuote(entry.error) + "}";
        }
        json += "]}\n";
        if (json.size() > kCacheLimit) throw std::runtime_error("discovery cache size limit");
        if (result_.inspectedDlls == 0 && previous_.size() == current_.size() && fs::exists(cache_)) return;
        const auto temporary = parent / (L"legacy-discovery-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
        HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create discovery cache");
        DWORD written = 0;
        const bool ok = WriteFile(handle, json.data(), DWORD(json.size()), &written, nullptr) && written == json.size();
        CloseHandle(handle);
        if (!ok || !MoveFileExW(temporary.c_str(), cache_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str()); throw std::runtime_error("cannot publish discovery cache");
        }
    } catch (const std::exception& e) {
        try { result_.diagnostics.push_back(std::string("legacy discovery cache unavailable: ") + e.what()); } catch (...) {}
    }
}
}
