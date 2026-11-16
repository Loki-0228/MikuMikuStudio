#include "manifest.hpp"
#include "legacy_discovery.hpp"
#include "loader_win32.hpp"
#include <charconv>
#include <iterator>

namespace mikumikustudio::plugins {
namespace {
void CheckId(const std::string& id) { if (!ValidId(id) || id == "mms" || id.rfind("mms.", 0) == 0) throw std::invalid_argument("invalid plugin ID: " + id); }
bool Plain(const fs::path& p) { auto a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT); }
void Unknown(const Json& j, std::initializer_list<const char*> known, const Log& log) {
    for (const auto& [key, value] : j.object)
        if (std::find(known.begin(), known.end(), key) == known.end() && log) log("ignored unknown field: " + key);
}
}
Version ParseVersion(const std::string& s) {
    Version v{}; size_t begin = 0;
    for (unsigned i = 0; i < 3; ++i) {
        auto end = s.find('.', begin); if (i == 2) end = s.size();
        if (end == std::string::npos || end == begin) throw std::invalid_argument("invalid version: " + s);
        auto result = std::from_chars(s.data() + begin, s.data() + end, v[i]);
        if (result.ec != std::errc{} || result.ptr != s.data() + end) throw std::invalid_argument("invalid version: " + s);
        begin = end + 1;
    }
    return v;
}
fs::path PackageFile(const fs::path& root, std::string_view relative) {
    if (relative.empty() || relative.find(':') != std::string_view::npos || relative.find('\0') != std::string_view::npos)
        throw std::invalid_argument("invalid package path");
    fs::path rel(Wide(relative)); if (rel.is_absolute() || rel.has_root_path()) throw std::invalid_argument("absolute package path");
    fs::path current = fs::absolute(root).lexically_normal();
    // Check ancestors too: a regular package below a junction is still outside
    // the declared discovery tree. Do not canonicalize through that junction.
    for (fs::path p = current; !p.empty();) {
        if (!Plain(p)) throw std::invalid_argument("missing path or reparse point: " + Utf8(p.native()));
        auto parent = p.parent_path(); if (parent == p) break; p = parent;
    }
    for (const auto& part : rel) {
        const auto name = part.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ' || name.find_first_of(L"<>\"|?*") != std::wstring::npos)
            throw std::invalid_argument("non-canonical package path");
        current /= part; if (!Plain(current)) throw std::invalid_argument("missing file or reparse point: " + Utf8(current.native()));
    }
    if (!fs::is_regular_file(current)) throw std::invalid_argument("entry is not a file"); return current;
}
fs::path LegacyMmdProviderFile(const fs::path& install) {
    if (!install.is_absolute()) throw std::invalid_argument("legacy MMDPlugin requires an absolute installation directory");
    // Walk from the root so a junction, non-directory or access error cannot
    // masquerade as a missing DLL and silently enable the other provider.
    for (const auto relative : {L"PLUGINS/LEGACY/MMDPlugin.dll",
            L"PLUGINS/legacy-mmdplugin/MMDPlugin.dll", L"legacy-mmdplugin/MMDPlugin.dll"}) {
        const fs::path parts(relative);
        auto candidate = install;
        bool missing = false;
        for (auto part = parts.begin(); part != parts.end(); ++part) {
            candidate /= *part;
            const auto attributes = GetFileAttributesW(candidate.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { missing = true; break; }
                throw std::runtime_error("cannot inspect legacy MMDPlugin provider path: " + Utf8(candidate.native()) +
                                         " (Win32 error " + std::to_string(error) + ")");
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                (std::next(part) != parts.end() && !(attributes & FILE_ATTRIBUTE_DIRECTORY)))
                throw std::invalid_argument("invalid legacy MMDPlugin provider path: " + Utf8(candidate.native()));
        }
        if (!missing) return PackageFile(install, Utf8(parts.native()));
    }
    return LegacyComponentFile(install, "MMDPlugin.dll");
}
fs::path LegacyComponentFile(const fs::path& install, std::string_view name) {
    if (!install.is_absolute()) throw std::invalid_argument("component installation path must be absolute");
    if (name != "MMDPlugin.dll" && fs::exists(install / Wide(name))) return PackageFile(install, name);
    const auto root = install / L"PLUGINS";
    std::vector<fs::path> found;
    size_t count = 0;
    if (fs::exists(root)) {
        if (!Plain(root)) throw std::invalid_argument("invalid component root");
        for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
            if (++count > 32768 || it.depth() > 64) throw std::invalid_argument("component discovery limit");
            if (!Plain(it->path())) { it.disable_recursion_pending(); continue; }
            if (Lower(it->path().filename().native()) == Lower(Wide(name)))
                found.push_back(PackageFile(install, Utf8(it->path().lexically_relative(install).native())));
        }
    }
    if (found.size() != 1) throw std::invalid_argument(std::string(name) +
        (found.empty() ? " compatibility component is missing" : " has ambiguous locations in PLUGINS"));
    return found.front();
}
Manifest ReadManifest(const fs::path& file, const Log& log) {
    Manifest m; m.file = fs::absolute(file).lexically_normal(); m.package = m.file.parent_path();
    PackageFile(m.package, Utf8(m.file.filename().native()));
    auto j = Json::Parse(ReadFile(file));
    if (j.At("schemaVersion").UInt() != 1) throw std::invalid_argument("unsupported manifest schema");
    m.id = j.At("id").String(); CheckId(m.id); m.name = j.At("name").String();
    if (m.name.empty()) throw std::invalid_argument("empty name");
    m.version = ParseVersion(j.At("version").String());
    const auto kind = j.At("kind").String();
    if (kind != "native" && j.object.count("startup"))
        throw std::invalid_argument("startup requests require a native plugin");
    if (kind == "legacy_mme") {
        m.kind = Manifest::Kind::LegacyMme; m.major = m.minor = 0;
        if (j.At("architecture").String() != "x64") throw std::invalid_argument("expected x64");
        m.minWindows = ParseVersion(j.At("minWindows").String());
        if (j.At("reloadPolicy").String() != "restart") throw std::invalid_argument("expected restart reloadPolicy");
        m.entry = PackageFile(m.package, j.At("entry").String());
        if (j.object.count("hack")) m.legacyHack = PackageFile(m.package, j.At("hack").String());
        if (SamePath(m.entry, m.legacyHack) || Lower(m.entry.extension().native()) != L".dll" ||
            (!m.legacyHack.empty() && Lower(m.legacyHack.extension().native()) != L".dll"))
            throw std::invalid_argument("legacy MME requires two distinct DLLs");
        for (const auto* field : {"abi", "capabilities", "requiresServices", "dependencies", "nativeDependencies"})
            if (j.object.count(field)) throw std::invalid_argument(std::string("legacy MME does not use native ABI field: ") + field);
        Unknown(j, {"schemaVersion", "id", "name", "version", "kind", "entry", "hack", "architecture", "minWindows", "reloadPolicy"}, log);
        return m;
    }
    if (kind == "legacy_mmdplugin") {
        m.kind = Manifest::Kind::LegacyMmdPlugin; m.major = 3; m.minor = 0;
        if (j.At("architecture").String() != "x64" || j.At("reloadPolicy").String() != "restart")
            throw std::invalid_argument("legacy MMDPlugin requires x64 and restart");
        const auto profile = j.At("profile").String();
        m.legacyProfile = profile;
        if (j.object.count("optIn")) {
            const auto& optIn = j.At("optIn");
            if (optIn.kind != Json::Boolean) throw std::invalid_argument("optIn must be a boolean");
            m.optIn = optIn.text == "true";
        }
        if (profile == "device_reset_v2" || profile == "effekseer_v2") m.major = 2;
        else if (profile == "first_person_camera_v4") m.major = 4;
        else if (profile != "window_observer_v3")
            throw std::invalid_argument("unsupported legacy MMDPlugin profile");
        m.minWindows = ParseVersion(j.At("minWindows").String());
        m.entry = PackageFile(m.package, j.At("entry").String());
        if (Lower(m.entry.extension().native()) != L".dll") throw std::invalid_argument("entry must be a DLL");
        m.legacyEntrySha256 = j.At("sha256").String();
        if (m.legacyEntrySha256.size() != 64 || m.legacyEntrySha256.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::invalid_argument("legacy MMDPlugin requires a lowercase SHA-256");
        constexpr std::string_view allowed[] = {"schemaVersion", "id", "name", "version", "kind", "entry", "sha256",
            "profile", "architecture", "minWindows", "reloadPolicy", "optIn"};
        for (const auto& field : j.object)
            if (std::find(std::begin(allowed), std::end(allowed), field.first) == std::end(allowed))
                throw std::invalid_argument("unsupported legacy MMDPlugin field: " + field.first);
        return m;
    }
    if (kind != "native") throw std::invalid_argument("unsupported plugin kind");
    if (j.object.count("optIn")) {
        const auto& optIn = j.At("optIn");
        if (optIn.kind != Json::Boolean) throw std::invalid_argument("optIn must be a boolean");
        m.optIn = optIn.text == "true";
    }
    if (j.object.count("startup")) {
        const auto& startup = j.At("startup");
        m.startupRenderBackend = startup.At("renderBackend").String();
        if (startup.object.size() != 1 || m.startupRenderBackend != "mme-d3d9on12")
            throw std::invalid_argument("startup requires the supported mme-d3d9on12 backend");
    }
    const auto& abi = j.At("abi"); m.major = abi.At("major").UInt(); m.minor = abi.At("minMinor").UInt();
    if (m.major != 1 || m.minor != 0) throw std::invalid_argument("unsupported plugin ABI");
    if (j.At("architecture").String() != "x64") throw std::invalid_argument("expected x64");
    m.minWindows = ParseVersion(j.At("minWindows").String());
    if (j.At("reloadPolicy").String() != "restart") throw std::invalid_argument("expected restart reloadPolicy");
    m.entry = PackageFile(m.package, j.At("entry").String());
    if (Lower(m.entry.extension().native()) != L".dll") throw std::invalid_argument("entry must be a DLL");
    for (const auto& cap : j.At("capabilities").Items()) {
        auto s = cap.String(); if (!ValidId(s)) throw std::invalid_argument("invalid capability"); m.capabilities.push_back(s);
    }
    if (m.capabilities.empty()) throw std::invalid_argument("empty capabilities");
    std::set<std::string> serviceIds, dependencyIds;
    for (const auto& s : j.At("requiresServices").Items()) {
        ServiceRequirement r{s.At("id").String(), s.At("major").UInt(), s.At("minMinor").UInt()};
        if (!ValidId(r.id) || !serviceIds.insert(r.id).second) throw std::invalid_argument("invalid/duplicate required service");
        m.services.push_back(r);
    }
    for (const auto& d : j.At("dependencies").Items()) {
        Dependency dep{d.At("id").String(), ParseVersion(d.At("minVersion").String()), ParseVersion(d.At("maxVersionExclusive").String())};
        CheckId(dep.id); if (dep.minimum >= dep.maximum || !dependencyIds.insert(dep.id).second) throw std::invalid_argument("invalid/duplicate dependency");
        m.dependencies.push_back(dep);
    }
    if (!m.startupRenderBackend.empty() && std::none_of(m.services.begin(), m.services.end(), [](const auto& service) {
            return service.id == "mms.render.mme.on12" && service.major == 1 && service.minor == 0;
        })) throw std::invalid_argument("mme-d3d9on12 requires mms.render.mme.on12 1.0");
    std::set<std::wstring> paths;
    for (const auto& d : j.At("nativeDependencies").Items()) {
        auto p = PackageFile(m.package, d.String());
        if (SamePath(p, m.entry) || Lower(p.extension().native()) != L".dll" || !paths.insert(Lower(p.native())).second)
            throw std::invalid_argument("invalid/duplicate native dependency");
        m.nativeDependencies.push_back(p);
    }
    Unknown(j, {"schemaVersion", "id", "name", "version", "kind", "abi", "entry", "architecture", "minWindows", "capabilities", "requiresServices", "dependencies", "nativeDependencies", "reloadPolicy", "optIn", "startup"}, log);
    return m;
}
Discovery Discover(const fs::path& directory, bool useCache) {
    Discovery result; const fs::path root = fs::absolute(directory).lexically_normal();
    auto log = [&](const std::string& s) { result.diagnostics.push_back(s); };
    auto fail = [&](const std::string& s) { log(s); result.errors.push_back(s); };
    if (!fs::exists(root)) return result;
    if (!Plain(root)) { fail("plugin root is a reparse point or unreadable"); return result; }
    const auto config = root / L"config.json";
    if (fs::exists(config)) try {
        PackageFile(root, "config.json"); auto j = Json::Parse(ReadFile(config)); DiscoveryConfig c;
        if (j.At("schemaVersion").UInt() != 1) throw std::invalid_argument("unsupported config schema");
        c.maxDepth = j.At("maxDepth").UInt();
        if (c.maxDepth > 64) throw std::invalid_argument("maxDepth exceeds 64");
        for (const auto& item : j.At("disabledPlugins").Items()) { auto id = item.String(); CheckId(id); c.disabled.insert(id); }
        if (j.object.count("enabledPlugins"))
            for (const auto& item : j.At("enabledPlugins").Items()) { auto id = item.String(); CheckId(id); c.enabled.insert(id); }
        Unknown(j, {"schemaVersion", "maxDepth", "disabledPlugins", "enabledPlugins"}, log); result.config = std::move(c);
    } catch (const std::exception& e) { log(std::string("config: ") + e.what() + "; using defaults"); }
    LegacyDiscovery automatic(root, result, useCache);
    size_t visited = 0;
    std::function<void(const fs::path&, uint32_t, bool)> scan = [&](const fs::path& dir, uint32_t depth, bool legacy) {
        if (++visited > 32768) throw std::runtime_error("discovery directory limit");
        if ((depth || legacy) && GetFileAttributesW((dir / L"plugin.json").c_str()) != INVALID_FILE_ATTRIBUTES) {
            try { result.packages.push_back(ReadManifest(dir / L"plugin.json", log)); }
            catch (const std::exception& e) { fail(Utf8((dir / L"plugin.json").native()) + ": " + e.what()); } return;
        }
        std::vector<fs::path> children;
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) { fail("directory read failed: " + Utf8(dir.native())); break; }
            children.push_back(it->path());
        }
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) { return Lower(a.native()) < Lower(b.native()); });
        // Flat manifests take precedence over auto detection too. If one is
        // invalid, do not bypass it by loading a neighboring DLL automatically.
        bool flatManifest = false;
        if (!depth) for (const auto& p : children) {
                auto name = Lower(p.filename().native()); constexpr std::wstring_view suffix = L".plugin.json";
                if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    flatManifest = true;
                    try { result.packages.push_back(ReadManifest(p, log)); }
                    catch (const std::exception& e) { fail(Utf8(p.native()) + ": " + e.what()); }
                }
        }
        for (const auto& p : children) {
            if (!Plain(p)) continue;
            if (fs::is_directory(p)) {
                // LEGACY is an explicit namespace, with its own depth origin.
                // Ordinary folders never infer legacy plugins from raw DLLs.
                if (!legacy && !depth && Lower(p.filename().native()) == L"legacy") scan(p, 0, true);
                else if (depth < result.config.maxDepth) scan(p, depth + 1, legacy);
            } else if (!flatManifest) {
                // Bundled components have explicit static markers. Directory
                // names are user-controlled; other loose DLLs are not inferred.
                const auto name = Lower(p.filename().native());
                if (name != L"dx12mmepatch.dll" && name != L"mmsfirstpersoncamera.dll") {
                    if (legacy && Lower(p.extension().native()) == L".dll") automatic.Inspect(p);
                    continue;
                }
                try {
                    const auto entry = PackageFile(dir, Utf8(p.filename().native()));
                    Manifest m; m.file = m.entry = entry; m.package = dir;
                    if (name == L"dx12mmepatch.dll") {
                        if (!HasPatch4MmsStartup(entry)) throw std::invalid_argument("missing Patch4MMS automatic-start marker");
                        m.id = "patch4mms"; m.name = "dx12mmepatch"; m.version = {0, 2, 0};
                        m.minWindows = {10, 0, 0}; m.startupRenderBackend = "mme-d3d9on12";
                        m.capabilities = {"render.observe"}; m.services = {{"mms.render.mme.on12", 1, 0}};
                    } else {
                        const auto exports = InspectLegacyExports(entry);
                        if (!exports.count("MMSLegacyCameraAdapterVersion") || !exports.count("version") ||
                                !exports.count("create4") || !exports.count("destroy4"))
                            throw std::invalid_argument("missing MMS camera adapter markers");
                        m.kind = Manifest::Kind::LegacyMmdPlugin; m.major = 4;
                        m.id = "org.mikumikustudio.legacy.first-person-camera"; m.name = "FirstPersonCamera (MMS)";
                        m.version = {1, 0, 0}; m.minWindows = {6, 1, 0};
                        m.legacyProfile = "first_person_camera_v4"; m.legacyEntrySha256 = Sha256(entry);
                    }
                    result.packages.push_back(std::move(m));
                } catch (const std::exception& e) { fail(Utf8(p.native()) + ": " + e.what()); }
            }
        }
    };
    try { scan(root, 0, false); } catch (const std::exception& e) { fail(e.what()); result.packages.clear(); }
    automatic.Save();
    return result;
}
std::vector<size_t> Resolve(const Discovery& d, const Log& log) {
    std::map<std::string, std::vector<size_t>> ids;
    for (size_t i = 0; i < d.packages.size(); ++i) ids[d.packages[i].id].push_back(i);
    std::vector<unsigned> state(d.packages.size()); std::vector<size_t> order;
    std::function<bool(size_t)> visit = [&](size_t n) {
        if (state[n]) return state[n] == 2;
        const auto& m = d.packages[n]; state[n] = 1;
        if (ids[m.id].size() != 1 || d.config.disabled.count(m.id) ||
            (m.optIn && !d.config.enabled.count(m.id))) {
            if (log) log(m.id + ": duplicate ID or disabled");
            state[n] = 3;
            return false;
        }
        bool ok = true;
        for (const auto& dep : m.dependencies) {
            auto i = ids.find(dep.id);
            if (i == ids.end() || i->second.size() != 1) { ok = false; break; }
            const size_t next = i->second.front(); const auto& target = d.packages[next];
            if (target.version < dep.minimum || target.version >= dep.maximum || !visit(next)) { ok = false; break; }
        }
        state[n] = ok ? 2 : 3; if (ok) order.push_back(n); else if (log) log(m.id + ": unavailable dependency, version conflict or cycle"); return ok;
    };
    for (const auto& [id, entries] : ids) for (size_t n : entries) visit(n);
    return order;
}
}
