// MAINTENANCE: regression-keep; actual bake-only staging and cache ownership.
// Included only by alembic_export_tests.cpp. Run a copied EXE from a fresh
// installation (no preexisting data directory), with an absolute fresh output
// directory and mode "storage". No dialog/keyboard/mouse/visual checks run.
namespace bake_storage {
namespace fs = std::filesystem;
struct EnvironmentValue {
    const wchar_t* name;
    std::wstring previous;
    bool present = false;
    explicit EnvironmentValue(const wchar_t* key) : name(key) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size) {
            previous.resize(size);
            Check(GetEnvironmentVariableW(name, previous.data(), size) == size - 1, "read inherited process environment");
            previous.resize(size - 1); present = true;
        }
    }
    void Set(const fs::path& path) { Check(_wputenv_s(name, path.c_str()) == 0, "set only this test process environment"); }
    ~EnvironmentValue() { Check(_wputenv_s(name, present ? previous.c_str() : L"") == 0, "restore inherited process environment"); }
};
fs::path InstallDirectory() {
    wchar_t executable[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, 32768);
    Check(size && size < 32768, "identify actual copied test executable");
    return fs::canonical(fs::path(executable).parent_path());
}
fs::path FreshOutput(const wchar_t* argument) {
    wchar_t desktop[128]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        !_wcsicmp(desktop, L"Default"), "storage host runs only on inherited Default desktop");
    const fs::path supplied(argument);
    Check(supplied.is_absolute(), "storage output must be explicitly absolute");
    const auto out = supplied.lexically_normal();
    Check(!fs::exists(out) && fs::is_directory(out.parent_path()), "storage output must be fresh with an existing parent");
    const auto install = InstallDirectory();
    Check(!fs::exists(install / L"data"), "storage requires an isolated copied installation with no inherited data tree");
    Check(out != install && out.parent_path() != install / L"data", "output is separate from the copied installation and its data root");
    Check(fs::create_directory(out), "create only the previously absent test output directory");
    return fs::canonical(out);
}
struct Environment {
    fs::path initialCwd = fs::current_path();
    fs::path install = InstallDirectory(), cwd, temporary;
    EnvironmentValue temp{L"TEMP"}, tmp{L"TMP"};
    explicit Environment(const fs::path& out) : cwd(out / L"foreign-cwd"), temporary(out / L"foreign-temp") {
        Check(fs::create_directory(cwd) && fs::create_directory(temporary), "create owned foreign cwd and TEMP directories");
        temp.Set(temporary); tmp.Set(temporary); Reenter();
        std::printf("storage install=%ls\noutput=%ls\ncwd=%ls\nTEMP=%ls\n", install.c_str(), out.c_str(), cwd.c_str(), temporary.c_str());
    }
    void Reenter() const {
        Check(SetCurrentDirectoryW(cwd.c_str()) != FALSE, "set own foreign cwd, including after the native PMX loader");
        Check(fs::equivalent(fs::current_path(), cwd) && !fs::equivalent(install, cwd) && !fs::equivalent(install, temporary),
            "actual current directory and TEMP are different from the copied EXE directory");
        wchar_t path[32768]{}; const DWORD size = GetTempPathW(32768, path);
        Check(size && size < 32768 && fs::equivalent(fs::path(path), temporary), "Windows temporary directory really resolves to the owned foreign TEMP");
    }
    ~Environment() { Check(SetCurrentDirectoryW(initialCwd.c_str()) != FALSE, "restore inherited cwd after hidden-host shutdown"); }
};
std::vector<unsigned char> Bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Check(bool(stream), "open an owned cache/output file for byte comparison");
    std::vector<unsigned char> result((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    Check(!stream.bad(), "read complete owned file bytes"); return result;
}
struct File { fs::path path; std::vector<unsigned char> bytes; };
std::vector<File> CacheFiles(const fs::path& root) {
    std::vector<File> files;
    if (fs::exists(root)) for (const auto& entry : fs::recursive_directory_iterator(root)) {
        Check(!entry.is_symlink(), "private cache contains no redirected test path");
        if (entry.is_regular_file()) { Check(entry.path().extension() == L".abc", "private cache file is an archive"); files.push_back({entry.path(), Bytes(entry.path())}); }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path < b.path; }); return files;
}
void SameFiles(const std::vector<File>& expected, const fs::path& root) {
    const auto actual = CacheFiles(root); Check(actual.size() == expected.size(), "failure/cancel preserves private cache file count");
    for (size_t i = 0; i < expected.size(); ++i)
        Check(actual[i].path == expected[i].path && actual[i].bytes == expected[i].bytes, "failure/cancel preserves the exact installed private archive path and bytes");
}
using Meshes = std::vector<deformed_mesh_capture::Mesh>;
std::vector<Meshes> Samples(const void* object) {
    std::vector<Meshes> output(3); std::string error;
    for (unsigned i = 0; i < output.size(); ++i)
        Check(mesh_bake_cache::Sample(object, double(i) / 30, output[i], error) && !output[i].empty(), "installed private archive remains sampleable after staging disappears");
    return output;
}
void SameSamples(const void* object, const std::vector<Meshes>& expected) {
    const auto actual = Samples(object); Check(actual.size() == expected.size(), "old cache keeps its sample count");
    for (size_t f = 0; f < actual.size(); ++f) { Check(actual[f].size() == expected[f].size(), "old cache keeps all mesh partitions");
        for (size_t i = 0; i < actual[f].size(); ++i) { const auto& a = actual[f][i]; const auto& b = expected[f][i];
            Check(a.name == b.name && a.vertexPatch == b.vertexPatch && a.sourceVertices == b.sourceVertices &&
                a.positions == b.positions && a.normals == b.normals && a.uvs == b.uvs && a.indices == b.indices,
                "cancel/failure preserves all old cached geometry and source mappings");
        }
    }
}
struct Scene {
    MMDApp& app; unsigned char* raw;
    std::unique_ptr<mdl::ModelRecord> model;
    std::vector<mdl::BoneRecord> bones;
    bone_scale::Snapshot scale;
    int frame, modified, playback;
    double cursor, start, end;
    std::uint64_t revision;
    Scene(MMDApp& a, unsigned char* p) : app(a), raw(p), model(new mdl::ModelRecord(*mdl::Mdl(p))),
        bones(model->boneTable, model->boneTable + model->boneCount), scale(bone_scale::Capture(p)),
        frame(a.CurrentFrame()), modified(a.SceneModified()), playback(a.PlaybackActive()), cursor(a.PlaybackCursorSeconds()),
        start(a.PlaybackStartSeconds()), end(a.PlaybackEndSeconds()), revision(a.SceneRevision()) {}
    void Same(bool published) const {
        const auto now = bone_scale::Capture(raw);
        Check(app.ModelSlot(0) == raw && !std::memcmp(model.get(), mdl::Mdl(raw), sizeof *model), "export restores the source model record and resource identities");
        Check(!std::memcmp(bones.data(), mdl::Mdl(raw)->boneTable, bones.size() * sizeof bones[0]) &&
            now.pose == scale.pose && now.keys == scale.keys && now.curves == scale.curves, "export restores bones and scale animation without pose edits");
        Check(app.CurrentFrame() == frame && app.PlaybackCursorSeconds() == cursor && app.PlaybackStartSeconds() == start &&
            app.PlaybackEndSeconds() == end && app.PlaybackActive() == playback, "export/cancel/path failure preserves playhead, playback clocks and state");
        if (!published) Check(app.SceneModified() == modified && app.SceneRevision() == revision, "cancel/failure/pure ABC creates no project modification");
    }
};
std::vector<fs::path> Partials(const fs::path& directory, const std::wstring& prefix) {
    std::vector<fs::path> output;
    if (fs::is_directory(directory)) for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().wstring().rfind(prefix, 0) == 0) { Check(entry.is_regular_file(), "actual staging path is a regular file"); output.push_back(entry.path()); }
    return output;
}
struct Staging {
    fs::path directory;
    std::wstring prefix;
    std::vector<fs::path> seen;
    unsigned calls = 0;
    Staging(fs::path parent, const wchar_t* target) : directory(std::move(parent)),
        prefix(std::wstring(target) + L".partial-" + std::to_wstring(GetCurrentProcessId()) + L"-") {}
    void Observe() {
        ++calls; const auto files = Partials(directory, prefix);
        Check(files.size() == 1 && files.front().parent_path() == directory, "progress sees exactly one actual partial in its required parent directory");
        const auto handle = CreateFileW(files.front().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(handle != INVALID_HANDLE_VALUE, "open the production partial itself while the real exporter owns it");
        BY_HANDLE_FILE_INFORMATION info{}; Check(GetFileInformationByHandle(handle, &info) != FALSE && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), "observed partial is an existing file handle");
        Check(CloseHandle(handle) != FALSE, "close only the probe's own partial-file handle");
        if (std::find(seen.begin(), seen.end(), files.front()) == seen.end()) seen.push_back(files.front());
    }
    void Cleared() const {
        Check(calls && seen.size() == 1 && Partials(directory, prefix).empty(), "all progress calls used one partial and completion removed it");
        for (const auto& path : seen) Check(!fs::exists(path), "the exact observed source partial was removed after success/cancel");
    }
};
void NoForeignStaging(const Environment& env) {
    for (const auto& root : {env.cwd, env.temporary}) for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto name = entry.path().filename().wstring();
        Check(name.find(L"mesh-bake") == std::wstring::npos && name.find(L"mesh_bake") == std::wstring::npos && name.find(L".partial-") == std::wstring::npos,
            "no bake staging/cache is redirected to foreign cwd or TEMP");
    }
}
void Run(MMDApp& app, const fs::path& out, Environment& env) {
    env.Reenter(); DWORD process = 0;
    Check(GetWindowThreadProcessId(app.MainWindow(), &process) == GetCurrentThreadId() && process == GetCurrentProcessId() &&
        !IsWindowVisible(app.MainWindow()) && app.Renderer() && app.Renderer()->device, "storage uses only its own hidden real-D3D host");
    Check(mesh_bake_cache::Empty(), "fresh hidden scene has no inherited bake bindings");
    app.CameraMode() = 0; for (int i = 0; i < 3; ++i) Frame(app);
    auto* raw = app.ModelSlot(0); Check(raw != nullptr, "storage uses the small loaded triangle only");
    const auto work = env.install / L"data" / L"mesh_bake_work", cache = env.install / L"data" / L"mesh_bake_cache";
    Check(!fs::exists(work) && !fs::exists(cache), "no prior run owns either tested mesh-bake directory");
    alembic_export::Options options; options.lastFrame = 2; options.writeAbc = false; options.bakeIntoProject = true;
    Staging successful(work, L"MMS-mesh-bake.abc"); Scene initial(app, raw);
    auto result = alembic_export::Export(app, {}, options, [&](int, int) { successful.Observe(); return true; });
    std::printf("storage bake success=%d baked=%d samples=%u error=%s\n", result.success, result.baked, result.samples, result.error.c_str());
    Check(result.success && result.baked && !result.abcWritten && result.samples == 3 && result.meshes && mesh_bake_cache::Has(raw), "real bake-only export installs all three triangle samples");
    initial.Same(true); successful.Cleared();
    Check(!fs::exists(work / L"MMS-mesh-bake.abc") && fs::is_empty(work), "bake-only does not publish a user ABC or retain work files");
    const auto oldFiles = CacheFiles(cache); Check(oldFiles.size() == 1 && !oldFiles.front().bytes.empty(), "Install retains exactly one actual nonempty archive below copied EXE/data/mesh_bake_cache");
    Check(oldFiles.front().path.parent_path().parent_path() == cache && oldFiles.front().path.parent_path().filename().wstring().rfind(L"mesh-cache-" + std::to_wstring(GetCurrentProcessId()) + L"-", 0) == 0,
        "installed archive is in the process-owned session directly below the expected data cache root");
    const auto oldSamples = Samples(raw);
    Check(oldSamples[0][0].positions.size() == 3 && oldSamples[2][0].positions.size() == 3, "installed triangle has three vertices at both sampled endpoints");
    const auto minimumX = [](const Meshes& meshes) {
        float value = meshes.front().positions.front()[0];
        for (const auto& mesh : meshes) for (const auto& position : mesh.positions) value = std::min(value, position[0]);
        return value;
    };
    Check(std::abs(minimumX(oldSamples[0])) < .005f && std::abs(minimumX(oldSamples[2]) - 2) < .02f,
        "installed archive contains the actual triangle's animated source samples independently of draw vertex order");
    NoForeignStaging(env);

    Staging cancelled(work, L"MMS-mesh-bake.abc"); Scene beforeCancel(app, raw);
    result = alembic_export::Export(app, {}, options, [&](int frame, int) { cancelled.Observe(); return frame < 1; });
    Check(result.cancelled && !result.success && !result.baked && !result.abcWritten && cancelled.calls >= 2, "cancel is reached after a real partial and one sampled frame");
    cancelled.Cleared(); beforeCancel.Same(false); SameFiles(oldFiles, cache); SameSamples(raw, oldSamples); NoForeignStaging(env);

    Check(fs::is_empty(work) && fs::remove(work), "remove only our now-empty work directory for the obstruction fixture");
    const std::string blocker = "owned mesh bake work obstruction";
    { std::ofstream stream(work, std::ios::binary); stream << blocker; stream.close(); Check(bool(stream), "create own regular-file work-root obstruction"); }
    const auto blockerBytes = Bytes(work); Scene beforeFailure(app, raw); unsigned failedProgress = 0;
    result = alembic_export::Export(app, {}, options, [&](int, int) { ++failedProgress; return true; });
    Check(!result.success && !result.cancelled && !result.baked && !result.abcWritten && !result.error.empty() && failedProgress == 0,
        "regular-file work-root obstruction rejects bake before sampling without fallback");
    Check(Bytes(work) == blockerBytes, "failed work-directory creation never rewrites the obstructing file");
    beforeFailure.Same(false); SameFiles(oldFiles, cache); SameSamples(raw, oldSamples); NoForeignStaging(env);

    options.writeAbc = true; options.bakeIntoProject = false;
    const auto userOutput = out / L"explicit-user-output.abc"; { std::ofstream existing(userOutput); existing << "owned previous destination"; }
    Staging explicitFile(out, L"explicit-user-output.abc"); Scene beforeExplicit(app, raw);
    result = alembic_export::Export(app, userOutput, options, [&](int, int) { explicitFile.Observe(); return true; });
    Check(result.success && result.abcWritten && !result.baked, "pure explicit ABC export bypasses the obstructed internal work root");
    explicitFile.Cleared(); beforeExplicit.Same(false); Read(userOutput, 3, 0, 1. / 30);
    Check(Bytes(work) == blockerBytes, "explicit user output leaves the unrelated work obstruction intact");
    SameFiles(oldFiles, cache); SameSamples(raw, oldSamples); NoForeignStaging(env);

    // All files under these two roots were created by this run. Release the
    // actual installed archive before replacing its now-empty shared cache root.
    mesh_bake_cache::Clear(); Check(mesh_bake_cache::Empty(), "release this run's installed private archive");
    Check(fs::is_empty(cache) && fs::remove(cache), "remove only our empty cache root after archive/session release");
    { std::ofstream stream(cache, std::ios::binary); stream << "owned cache-root obstruction"; stream.close(); Check(bool(stream), "create own regular-file cache-root obstruction"); }
    const auto cacheBlocker = Bytes(cache);
    const auto secondOutput = out / L"explicit-cache-blocked.abc";
    Staging cacheBlocked(out, L"explicit-cache-blocked.abc"); Scene beforeCacheBlocked(app, raw);
    result = alembic_export::Export(app, secondOutput, options, [&](int, int) { cacheBlocked.Observe(); return true; });
    Check(result.success && result.abcWritten && !result.baked && mesh_bake_cache::Empty(), "pure user ABC still exports with both internal cache/work roots occupied by files");
    cacheBlocked.Cleared(); beforeCacheBlocked.Same(false); Read(secondOutput, 3, 0, 1. / 30);
    Check(Bytes(cache) == cacheBlocker && Bytes(work) == blockerBytes, "pure explicit export neither replaces nor falls back from either cache obstruction");
    NoForeignStaging(env);
    std::puts("PASS: isolated EXE/data bake staging, installed cache, cancel, obstructed roots and explicit ABC output");
}
} // namespace bake_storage
