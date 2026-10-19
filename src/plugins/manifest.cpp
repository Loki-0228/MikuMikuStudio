#include "manifest.hpp"
#include <charconv>

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
Manifest ReadManifest(const fs::path& file, const Log& log) {
    Manifest m; m.file = fs::absolute(file).lexically_normal(); m.package = m.file.parent_path();
    PackageFile(m.package, Utf8(m.file.filename().native()));
    auto j = Json::Parse(ReadFile(file));
    if (j.At("schemaVersion").UInt() != 1) throw std::invalid_argument("unsupported manifest schema");
    m.id = j.At("id").String(); CheckId(m.id); m.name = j.At("name").String();
    if (m.name.empty()) throw std::invalid_argument("empty name");
    m.version = ParseVersion(j.At("version").String());
    if (j.At("kind").String() != "native") throw std::invalid_argument("unsupported plugin kind");
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
    std::set<std::wstring> paths;
    for (const auto& d : j.At("nativeDependencies").Items()) {
        auto p = PackageFile(m.package, d.String());
        if (SamePath(p, m.entry) || Lower(p.extension().native()) != L".dll" || !paths.insert(Lower(p.native())).second)
            throw std::invalid_argument("invalid/duplicate native dependency");
        m.nativeDependencies.push_back(p);
    }
    Unknown(j, {"schemaVersion", "id", "name", "version", "kind", "abi", "entry", "architecture", "minWindows", "capabilities", "requiresServices", "dependencies", "nativeDependencies", "reloadPolicy"}, log);
    return m;
}
Discovery Discover(const fs::path& directory) {
    Discovery result; const fs::path root = fs::absolute(directory).lexically_normal();
    auto log = [&](const std::string& s) { result.diagnostics.push_back(s); };
    if (!fs::exists(root)) return result;
    if (!Plain(root)) { log("plugin root is a reparse point or unreadable"); return result; }
    const auto config = root / L"config.json";
    if (fs::exists(config)) try {
        PackageFile(root, "config.json"); auto j = Json::Parse(ReadFile(config)); DiscoveryConfig c;
        if (j.At("schemaVersion").UInt() != 1) throw std::invalid_argument("unsupported config schema");
        c.maxDepth = j.At("maxDepth").UInt();
        if (c.maxDepth > 64) throw std::invalid_argument("maxDepth exceeds 64");
        for (const auto& item : j.At("disabledPlugins").Items()) { auto id = item.String(); CheckId(id); c.disabled.insert(id); }
        Unknown(j, {"schemaVersion", "maxDepth", "disabledPlugins"}, log); result.config = std::move(c);
    } catch (const std::exception& e) { log(std::string("config: ") + e.what() + "; using defaults"); }
    size_t visited = 0;
    std::function<void(const fs::path&, uint32_t)> scan = [&](const fs::path& dir, uint32_t depth) {
        if (++visited > 32768) throw std::runtime_error("discovery directory limit");
        if (depth && Plain(dir / L"plugin.json") && fs::is_regular_file(dir / L"plugin.json")) {
            try { result.packages.push_back(ReadManifest(dir / L"plugin.json", log)); }
            catch (const std::exception& e) { log(Utf8((dir / L"plugin.json").native()) + ": " + e.what()); } return;
        }
        std::vector<fs::path> children;
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) { log("directory read failed: " + Utf8(dir.native())); break; }
            children.push_back(it->path());
        }
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) { return Lower(a.native()) < Lower(b.native()); });
        for (const auto& p : children) {
            if (!Plain(p)) continue;
            if (fs::is_directory(p)) { if (depth < result.config.maxDepth) scan(p, depth + 1); }
            else if (!depth) {
                auto name = Lower(p.filename().native()); constexpr std::wstring_view suffix = L".plugin.json";
                if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    try { result.packages.push_back(ReadManifest(p, log)); }
                    catch (const std::exception& e) { log(Utf8(p.native()) + ": " + e.what()); }
                }
            }
        }
    };
    try { scan(root, 0); } catch (const std::exception& e) { log(e.what()); result.packages.clear(); }
    return result;
}
std::vector<size_t> Resolve(const Discovery& d, const Log& log) {
    std::map<std::string, std::vector<size_t>> ids;
    for (size_t i = 0; i < d.packages.size(); ++i) ids[d.packages[i].id].push_back(i);
    std::vector<unsigned> state(d.packages.size()); std::vector<size_t> order;
    std::function<bool(size_t)> visit = [&](size_t n) {
        if (state[n]) return state[n] == 2;
        const auto& m = d.packages[n]; state[n] = 1;
        if (ids[m.id].size() != 1 || d.config.disabled.count(m.id)) { if (log) log(m.id + ": duplicate ID or disabled"); state[n] = 3; return false; }
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
