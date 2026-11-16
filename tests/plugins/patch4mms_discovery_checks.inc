// Included by the startup protocol test; discovery must not execute candidates.
void PatchDiscovery(const fs::path& dll, const fs::path& markerless, const fs::path& camera) {
    const auto root = fs::temp_directory_path() / (L"mms-patch-discovery-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(root), "fresh discovery fixture");
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); } } cleanup{root};
    const auto plugins = root / L"PLUGINS";
    auto package = plugins / L"dx12mmepatch";
    fs::create_directories(package);
    fs::copy_file(dll, package / L"dx12mmepatch.dll");
    auto found = Discover(plugins, false);
    Check(found.errors.empty() && found.packages.size() == 1, "bundled patch discovered without JSON");
    const auto& m = found.packages.front();
    Check(m.id == "patch4mms" && m.kind == Manifest::Kind::Native && !m.optIn &&
        m.startupRenderBackend == "mme-d3d9on12" && m.services.size() == 1 &&
        m.services[0].id == "mms.render.mme.on12", "automatic patch carries the startup contract");
    Check(Resolve(found, {}).size() == 1 && !fs::exists(package / L"plugin.json") &&
        !GetModuleHandleW(L"dx12mmepatch.dll"), "discovery enables only metadata and writes no manifest");
    const auto config = plugins / L"config.json";
    std::ofstream(config) << "{\"schemaVersion\":1,\"maxDepth\":2,\"disabledPlugins\":[\"patch4mms\"]}";
    Check(Resolve(Discover(plugins, false), {}).empty(), "blacklist wins for automatic patch");
    std::ofstream(config) << "{\"schemaVersion\":1,\"maxDepth\":0,\"disabledPlugins\":[]}";
    Check(Discover(plugins, false).packages.empty(), "zero depth excludes automatic patch package");
    fs::remove(config);
    std::ofstream(package / L"plugin.json") << "{}";
    found = Discover(plugins, false);
    Check(found.packages.empty() && !found.errors.empty(), "invalid explicit manifest blocks automatic fallback");
    fs::remove(package / L"plugin.json");
    const auto group = plugins / L"group";
    fs::create_directory(group);
    fs::rename(package, group / L"dx12mmepatch");
    package = group / L"dx12mmepatch";
    Check(Discover(plugins, false).packages.size() == 1, "nested package within default depth is found");
    std::ofstream(config) << "{\"schemaVersion\":1,\"maxDepth\":1,\"disabledPlugins\":[]}";
    Check(Discover(plugins, false).packages.empty(), "maxDepth limits nested native patch");
    fs::remove(config);
    fs::rename(package, group / L"ordinary");
    package = group / L"ordinary";
    Check(Discover(plugins, false).packages.size() == 1, "renaming the package folder preserves static marker discovery");
    fs::rename(package, group / L"dx12mmepatch");
    package = group / L"dx12mmepatch";
    fs::copy_file(markerless, package / L"dx12mmepatch.dll", fs::copy_options::overwrite_existing);
    found = Discover(plugins, false);
    Check(found.packages.empty() && !found.errors.empty(), "renamed DLL without direct marker is rejected");
    fs::copy_file(dll, package / L"dx12mmepatch.dll", fs::copy_options::overwrite_existing);
    fs::create_directories(plugins / L"another" / L"dx12mmepatch");
    fs::copy_file(dll, plugins / L"another" / L"dx12mmepatch" / L"dx12mmepatch.dll");
    found = Discover(plugins, false);
    Check(found.packages.size() == 2 && Resolve(found, {}).empty(), "duplicate patch identities do not load");
    fs::remove(plugins / L"another" / L"dx12mmepatch" / L"dx12mmepatch.dll");
    const auto cameraFolder = plugins / L"tools" / L"my-camera";
    fs::create_directories(cameraFolder);
    fs::copy_file(camera, cameraFolder / L"MMSFirstPersonCamera.dll");
    found = Discover(plugins, false);
    Check(found.errors.empty() && found.packages.size() == 2 && Resolve(found, {}).size() == 2,
        "both bundled plugins auto-enable in renamed nested folders without JSON");
    const auto it = std::find_if(found.packages.begin(), found.packages.end(), [](const auto& item) {
        return item.id == "org.mikumikustudio.legacy.first-person-camera";
    });
    Check(it != found.packages.end() && it->major == 4 && !it->optIn &&
        it->legacyProfile == "first_person_camera_v4" && it->legacyEntrySha256 == Sha256(camera),
        "camera uses verified adapter profile and current DLL identity");
    Check(!GetModuleHandleW(L"MMSFirstPersonCamera.dll") && !fs::exists(cameraFolder / L"plugin.json"),
        "camera discovery does not execute code or write a manifest");
    std::ofstream(config) << "{\"schemaVersion\":1,\"maxDepth\":2,\"disabledPlugins\":[\"org.mikumikustudio.legacy.first-person-camera\"]}";
    Check(Resolve(Discover(plugins, false), {}).size() == 1, "camera blacklist remains effective");
    std::ofstream(config) << "{\"schemaVersion\":1,\"maxDepth\":1,\"disabledPlugins\":[]}";
    Check(Discover(plugins, false).packages.empty(), "depth bounds both renamed packages");
    fs::remove(config);
    fs::copy_file(markerless, cameraFolder / L"MMSFirstPersonCamera.dll", fs::copy_options::overwrite_existing);
    found = Discover(plugins, false);
    Check(found.packages.size() == 1 && !found.errors.empty(), "a filename alone cannot identify the camera adapter");
    fs::remove(cameraFolder / L"MMSFirstPersonCamera.dll");
    fs::copy_file(camera, plugins / L"MMSFirstPersonCamera.dll");
    fs::copy_file(dll, plugins / L"dx12mmepatch.dll");
    std::ofstream(config) << "{\"schemaVersion\":1,\"maxDepth\":0,\"disabledPlugins\":[]}";
    found = Discover(plugins, false);
    Check(found.packages.size() == 2 && Resolve(found, {}).size() == 2,
        "bundled marked DLLs can also be placed directly in the plugin root");
    fs::remove(config);
    fs::remove(plugins / L"dx12mmepatch.dll");
    fs::remove(plugins / L"MMSFirstPersonCamera.dll");
    fs::remove(package / L"dx12mmepatch.dll");
    const auto legacyGroup = plugins / L"LEGACY" / L"bundled";
    fs::create_directories(legacyGroup);
    fs::copy_file(dll, legacyGroup / L"dx12mmepatch.dll");
    fs::copy_file(camera, legacyGroup / L"MMSFirstPersonCamera.dll");
    found = Discover(plugins, false);
    Check(found.errors.empty() && found.packages.size() == 2 && Resolve(found, {}).size() == 2,
        "bundled markers also auto-enable inside the legacy namespace");
    fs::remove(legacyGroup / L"dx12mmepatch.dll");
    fs::remove(legacyGroup / L"MMSFirstPersonCamera.dll");
    fs::copy_file(markerless, plugins / L"ordinary.dll");
    Check(Discover(plugins, false).packages.empty(), "ordinary loose DLLs do not gain automatic discovery");
    std::cout << "PASS manifest-free bundled discovery, folder renaming, depth, blacklist and explicit precedence\n";
}
