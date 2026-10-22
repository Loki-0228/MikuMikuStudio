// Real Ogawa and filesystem checks, included after the ordinary mesh fixtures.
namespace installation_storage {
namespace fs = std::filesystem;
namespace cache = mikumikustudio::mesh_bake_cache;
constexpr char kInstallationMarker[] = "MMS mesh cache test installation v1\n";
constexpr char kForeign[] = "foreign file inside a private session\n";
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE input = nullptr) : value(input) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
fs::path Executable() {
    wchar_t path[32768]{}; const auto length = GetModuleFileNameW(nullptr, path, 32768);
    Check(length && length < 32768, "identify only this test executable"); return fs::path(path).lexically_normal();
}
bool Same(const fs::path& left, const fs::path& right) { return !_wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()); }
bool Inside(const fs::path& path, const fs::path& root) {
    const auto normalized = path.lexically_normal(), base = root.lexically_normal();
    auto current = normalized.begin();
    for (const auto& part : base) { if (current == normalized.end() || _wcsicmp(current->c_str(), part.c_str())) return false; ++current; }
    return true;
}
void Installation(const fs::path& executable) {
    Check(executable.is_absolute() && fs::is_regular_file(executable), "specialized cache checks require an absolute copied executable");
    const auto folder = executable.parent_path();
    for (auto current = folder; !current.empty();) {
        const auto attributes = GetFileAttributesW(current.c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "disposable installation ancestors are ordinary directories");
        const auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
    const auto marker = folder / L"mesh-cache-test-installation.txt";
    const auto attributes = GetFileAttributesW(marker.c_str());
    Check(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
          ReadFileBytes(marker) == kInstallationMarker, "refuse cache mutation outside an explicitly marked disposable copied installation");
}
fs::path Fresh(const fs::path& requested) {
    Check(requested.is_absolute(), "cache evidence directory must be explicitly absolute");
    const auto out = requested.lexically_normal(), executable = Executable(); std::error_code error;
    Check(!fs::exists(out, error) && !error && out != out.root_path() && !Same(out, executable.parent_path()) &&
          !Inside(out, executable.parent_path() / L"data"), "refuse existing output, executable/data directories and all overwrites");
    Check(fs::create_directories(out, error) && !error, "create a fresh cache evidence directory"); return out;
}
void NewText(const fs::path& file, const std::string& text) {
    Handle handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(handle.value != INVALID_HANDLE_VALUE, "create only a new owned marker or blocking file"); DWORD written = 0;
    Check(WriteFile(handle.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() && FlushFileBuffers(handle.value), "write the complete owned marker or blocking file");
}
struct Environment {
    struct Variable {
        const wchar_t* name; std::wstring before; bool present = false;
        explicit Variable(const wchar_t* key) : name(key) {
            const auto size = GetEnvironmentVariableW(name, nullptr, 0);
            if (size) { before.resize(size); Check(GetEnvironmentVariableW(name, before.data(), size) == size - 1, "capture only this process's previous temporary-directory setting"); before.resize(size - 1); present = true; }
        }
        ~Variable() { SetEnvironmentVariableW(name, present ? before.c_str() : nullptr); }
    } temp{L"TEMP"}, tmp{L"TMP"};
    fs::path before, working, temporary, alternateTemporary;
    explicit Environment(const fs::path& out) : before(fs::current_path()), working(out / L"different-cwd"), temporary(out / L"different-temp"), alternateTemporary(out / L"different-tmp") {
        Check(fs::create_directory(working) && fs::create_directory(temporary) && fs::create_directory(alternateTemporary), "create three distinct owned cwd, TEMP and TMP fixtures");
        Check(SetCurrentDirectoryW(working.c_str()) && SetEnvironmentVariableW(L"TEMP", temporary.c_str()) && SetEnvironmentVariableW(L"TMP", alternateTemporary.c_str()), "change only this test process's cwd, TEMP and TMP");
    }
    ~Environment() { SetCurrentDirectoryW(before.c_str()); }
    void NoFallback() const {
        Check(!fs::exists(working / L"MikuMikuStudio") && !fs::exists(temporary / L"MikuMikuStudio") &&
              !fs::exists(alternateTemporary / L"MikuMikuStudio") && !fs::exists(working / L"data"), "no cache root appears under changed cwd, TEMP or TMP");
    }
};
std::vector<fs::path> Copies() {
    std::vector<fs::path> result;
    for (const auto& session : SessionDirectories()) {
        std::error_code error;
        for (const auto& file : fs::directory_iterator(session, error))
            if (file.is_regular_file() && file.path().extension() == L".abc") result.push_back(file.path());
        Check(!error, "enumerate this process's actual private session copies");
    }
    return result;
}
fs::path Installed(Scene& scene, const fs::path& source, float value) {
    WriteArchive(source, {{kModelChild, value}}, 3, 30.0, 0.0); const auto sourceBytes = ReadFileBytes(source); std::string error;
    Check(Install(scene.app, source, error), "install real archive into the copied installation: " + error);
    const auto copies = Copies();
    Check(copies.size() == 1 && Same(copies[0].parent_path().parent_path(), SharedTempRoot()), "actual private copy is inside EXE/data/mesh_bake_cache, independently of the changed environment");
    Check(ReadFileBytes(copies[0]) == sourceBytes, "private archive is an exact copy of the real source");
    Check(fs::remove(source) && !fs::exists(source), "remove the original external source after its private copy is established");
    ExpectTriangle(SampleAt(scene.Model0(), 0, "source removed")[0], kModelChild, value, "private archive remains sampleable after the original source is removed"); return copies[0];
}
struct Evidence { fs::path root, session, copy, foreign; DWORD process = 0; };
void EvidenceFile(const fs::path& path, const Evidence& value) {
    Check(!fs::exists(path), "peer evidence is a new owned file"); std::ofstream file(path, std::ios::binary);
    const auto field = [&](const fs::path& item) { const auto text = item.wstring(); const auto size = static_cast<std::uint32_t>(text.size()); Check(size < 32768, "peer evidence path length is bounded"); file.write(reinterpret_cast<const char*>(&size), 4); file.write(reinterpret_cast<const char*>(text.data()), size * sizeof(wchar_t)); };
    file.write("MMSMC1", 6); file.write(reinterpret_cast<const char*>(&value.process), sizeof value.process);
    field(value.root); field(value.session); field(value.copy); field(value.foreign); file.close(); Check(!file.fail(), "publish complete peer evidence before signaling ready");
}
Evidence ReadEvidence(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); char magic[6]{}; Evidence result;
    Check(file.read(magic, 6) && !std::memcmp(magic, "MMSMC1", 6) && file.read(reinterpret_cast<char*>(&result.process), sizeof result.process), "read this exact peer-evidence format");
    const auto field = [&]() { std::uint32_t size = 0; Check(bool(file.read(reinterpret_cast<char*>(&size), 4)) && size < 32768, "read a bounded peer path"); std::wstring text(size, L'\0'); Check(bool(file.read(reinterpret_cast<char*>(text.data()), size * sizeof(wchar_t))) && text.find(L'\0') == std::wstring::npos, "read a complete peer path without embedded NULs"); return fs::path(text); };
    result.root = field(); result.session = field(); result.copy = field(); result.foreign = field();
    Check(file.peek() == std::char_traits<char>::eof(), "peer evidence has no unrecognized trailing data"); return result;
}
void Wait(HANDLE event, const char* why) { Check(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, why); }
std::wstring EventName(const std::wstring& base, const wchar_t* suffix) { return base + L"-" + suffix; }
int Peer(const fs::path& requested, const std::wstring& base, const std::wstring& role) {
    Installation(Executable()); const auto out = Fresh(requested); Environment environment(out);
    Check(role == L"same" || role == L"cross", "peer role is explicitly bounded");
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(base, L"ready").c_str()));
    Handle go(OpenEventW(SYNCHRONIZE, FALSE, EventName(base, L"go").c_str()));
    Handle done(OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(base, L"done").c_str()));
    Check(ready.value && go.value && done.value, "peer opens only its parent's declared synchronization events");
    auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{}; ResetSlots(*app); Scene scene(*app); scene.BindModel0();
    const auto copy = Installed(scene, out / L"peer-source.abc", 123.f); const auto bytes = ReadFileBytes(copy);
    const auto foreign = copy.parent_path() / L"peer-foreign.txt"; NewText(foreign, kForeign);
    EvidenceFile(out / L"lease.bin", {SharedTempRoot(), copy.parent_path(), copy, foreign, GetCurrentProcessId()}); environment.NoFallback();
    Check(SetEvent(ready.value) != FALSE, "peer signals its live private lease"); Wait(go.value, "peer receives the declared cleanup turn");
    Check(fs::exists(copy) && ReadFileBytes(copy) == bytes && ReadFileBytes(foreign) == kForeign, "another process's Clear and lease release preserve this live process's files");
    ExpectTriangle(SampleAt(scene.Model0(), 0, "peer after parent cleanup")[0], kModelChild, 123.f, "peer still samples its archive after the parent clears its own cache");
    Clear(); Check(Empty() && !fs::exists(copy) && ReadFileBytes(foreign) == kForeign && fs::is_directory(copy.parent_path()), "peer cleanup deletes only its own recorded copy and preserves its planted foreign file");
    environment.NoFallback(); Check(SetEvent(done.value) != FALSE, "peer reports completed cleanup"); return 0;
}
struct Child {
    PROCESS_INFORMATION process{};
    ~Child() { if (process.hThread) CloseHandle(process.hThread); if (process.hProcess) CloseHandle(process.hProcess); }
    void Start(const fs::path& executable, const fs::path& out, const std::wstring& base, const wchar_t* role) {
        const auto quote = [](const std::wstring& text) { Check(text.find(L'"') == std::wstring::npos, "owned process argument contains no quote"); return L"\"" + text + L"\""; };
        std::wstring command = quote(executable.wstring()) + L" --storage-peer " + quote(out.wstring()) + L" " + quote(base) + L" " + quote(role);
        STARTUPINFOW startup{}; startup.cb = sizeof startup; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "launch only the declared copied test executable as a hidden peer");
    }
    void Ready(HANDLE ready) const { const HANDLE waits[]{ready, process.hProcess}; Check(WaitForMultipleObjects(2, waits, FALSE, 30000) == WAIT_OBJECT_0, "peer reached its live lease without exiting or timing out"); }
    void Finish(HANDLE done) const { const HANDLE waits[]{done, process.hProcess}; const auto signaled = WaitForMultipleObjects(2, waits, FALSE, 30000); Check(signaled == WAIT_OBJECT_0 || signaled == WAIT_OBJECT_0 + 1, "peer reaches its terminal cleanup state within the bound"); Wait(process.hProcess, "peer process exits after cleanup"); DWORD code = 1; Check(GetExitCodeProcess(process.hProcess, &code) && code == 0 && WaitForSingleObject(done, 0) == WAIT_OBJECT_0, "peer exits successfully only after signaling actual cleanup completion"); }
};
void BlockedData(const fs::path& out, Scene& scene, const Environment& environment) {
    const auto data = Executable().parent_path() / L"data";
    Check(!fs::exists(data), "specialized parent starts in a fresh copied installation without data");
    const auto retained = out / fs::u8path(std::string("retained") + cache::kSidecarSuffix); WriteArchive(retained, {{kModelChild, 42.f}}, 3, 30.0, 0.0);
    const auto retainedBytes = ReadFileBytes(retained); extension::Document original;
    extension::Set(original, kRecordKey, BuildValue({{0, 0, retained.filename().u8string()}})); std::string error;
    const auto reference = out / L"reference.pmm";
    Check(Load(scene.app, reference, original, error) && !fs::exists(data), "load a real persistent archive without preparing any installation cache directory: " + error);
    auto snapshot = cache::CaptureSave(scene.app); NewText(data, "owned data-root blocker\n");
    const auto candidate = out / L"blocked-candidate.abc"; WriteArchive(candidate, {{kModelChild, 99.f}}, 3, 30.0, 0.0); const auto candidateBytes = ReadFileBytes(candidate);
    Check(!Install(scene.app, candidate, error) && !error.empty() && Has(scene.Model0()), "file-blocked data root refuses installation without replacing the live archive or falling back");
    ExpectTriangle(SampleAt(scene.Model0(), 0, "failed cache installation")[0], kModelChild, 42.f, "failed private-copy preparation preserves actual native cache sampling");
    Check(ReadFileBytes(data) == "owned data-root blocker\n" && ReadFileBytes(candidate) == candidateBytes && ReadFileBytes(retained) == retainedBytes, "failed preparation preserves the blocker, old source and candidate bytes");
    Clear(); Check(Empty(), "live clear retires bindings while the immutable snapshot keeps its persistent archive");
    extension::Document document; const auto project = out / L"blocked-snapshot.pmm";
    Check(snapshot(project, document, error) && extension::Find(document, kRecordKey), "retained save snapshot remains publishable after preparation failure and live Clear: " + error);
    WriteProjectFile(project, document); extension::Document reopened; std::uint64_t standard = 0;
    Check(ReadProjectFile(project, reopened, standard) && Load(scene.app, project, reopened, error), "unchanged MMS extension and published sidecar reopen after blocked cache preparation");
    ExpectTriangle(SampleAt(scene.Model0(), 0, "reopened blocked snapshot")[0], kModelChild, 42.f, "failed candidate never replaces the immutable saved mesh");
    snapshot = {}; Clear(); Check(ReadFileBytes(retained) == retainedBytes && ReadFileBytes(data) == "owned data-root blocker\n", "last snapshot release does not delete persistent user sidecars or the blocking file");
    environment.NoFallback(); Check(DeleteFileW(data.c_str()) && !fs::exists(data), "remove only the exact blocking file this test created");
    Trace("blocked data root preserves native state, immutable snapshot and all source bytes without fallback");
}
void Concurrent(const fs::path& out, Scene& scene, const fs::path& peerExecutable, bool sameInstallation) {
    const auto role = sameInstallation ? L"same" : L"cross"; const auto round = out / role; Check(fs::create_directory(round), "create fresh synchronized cache round"); Environment environment(round);
    const auto expectedRoot = Executable().parent_path() / L"data" / L"mesh_bake_cache";
    Check(Same(SharedTempRoot(), expectedRoot), "changed cwd/TEMP/TMP cannot change the independently derived cache root");
    const auto copy = Installed(scene, round / L"caller-source.abc", 71.f); const auto ownBytes = ReadFileBytes(copy);
    const auto foreign = copy.parent_path() / (std::wstring(L"caller-foreign-") + role + L".txt"); NewText(foreign, kForeign);
    auto snapshot = cache::CaptureSave(scene.app);
    const auto base = L"Local\\MMS.MeshCache.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"." + role;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, EventName(base, L"ready").c_str())); Check(ready.value && GetLastError() != ERROR_ALREADY_EXISTS, "create a new private ready event");
    Handle go(CreateEventW(nullptr, TRUE, FALSE, EventName(base, L"go").c_str())); Check(go.value && GetLastError() != ERROR_ALREADY_EXISTS, "create a new private cleanup event");
    Handle done(CreateEventW(nullptr, TRUE, FALSE, EventName(base, L"done").c_str())); Check(done.value && GetLastError() != ERROR_ALREADY_EXISTS, "create a new private completed event");
    const auto peerOut = round / L"peer-output"; Child child; child.Start(peerExecutable, peerOut, base, role); child.Ready(ready.value);
    const auto peer = ReadEvidence(peerOut / L"lease.bin"); const auto expectedPeerRoot = peerExecutable.parent_path() / L"data" / L"mesh_bake_cache";
    Check(peer.process == child.process.dwProcessId && peer.process != GetCurrentProcessId() && Same(peer.root, expectedPeerRoot) &&
          Same(peer.session.parent_path(), peer.root) && Same(peer.copy.parent_path(), peer.session) && Same(peer.foreign.parent_path(), peer.session) &&
          !Same(peer.session, copy.parent_path()), "two actual processes expose distinct private sessions under their own independently expected installation roots");
    Check(Same(peer.root, expectedRoot) == sameInstallation, "same-installation peers share only the root; distinct installations never share their cache root");
    const auto peerBytes = ReadFileBytes(peer.copy); Check(ReadFileBytes(peer.foreign) == kForeign, "peer's active archive and planted foreign sentinel are present");
    Clear(); Check(Empty() && fs::exists(copy) && ReadFileBytes(copy) == ownBytes && ReadFileBytes(peer.copy) == peerBytes, "Clear keeps captured private files alive and cannot delete another process's lease");
    extension::Document document; std::string error; const auto project = round / L"frozen.pmm"; bool published = false;
    std::thread worker([&] { published = snapshot(project, document, error); }); worker.join();
    Check(published && extension::Find(document, kRecordKey), "real worker publishes the immutable archive after live Clear and original-source deletion: " + error);
    WriteProjectFile(project, document); extension::Document reopened; std::uint64_t standard = 0;
    Check(ReadProjectFile(project, reopened, standard) && Load(scene.app, project, reopened, error), "real MMS-only sidecar record reopens the captured private mesh");
    ExpectTriangle(SampleAt(scene.Model0(), 0, "retained snapshot reopened")[0], kModelChild, 71.f, "snapshot retained the original source samples rather than another process's archive");
    snapshot = {}; Clear();
    Check(!fs::exists(copy) && ReadFileBytes(foreign) == kForeign && ReadFileBytes(peer.copy) == peerBytes && ReadFileBytes(peer.foreign) == kForeign, "last local lease release deletes only its own recorded copy, preserving both foreign sentinels and the peer archive");
    Check(SetEvent(go.value) != FALSE, "give the peer its explicit cleanup turn"); child.Finish(done.value);
    Check(!fs::exists(peer.copy) && ReadFileBytes(peer.foreign) == kForeign && ReadFileBytes(foreign) == kForeign && fs::is_directory(peer.root) && fs::is_directory(expectedRoot), "peer cleanup keeps both roots and every foreign file while removing its own private copy");
    environment.NoFallback(); Trace(sameInstallation ? "same installation: concurrent process sessions and cleanup are isolated" : "two copied installations: private roots, active leases and cleanup remain independent");
}
int Run(const fs::path& requested, const fs::path& secondExecutable) {
    const auto executable = Executable(); Installation(executable); Installation(secondExecutable);
    Check(!Same(executable.parent_path(), secondExecutable.parent_path()) && !fs::equivalent(executable.parent_path(), secondExecutable.parent_path()), "two separately copied installations are required for the complete storage cohort");
    const auto out = Fresh(requested); Environment environment(out);
    auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{}; ResetSlots(*app); Scene scene(*app); scene.BindModel0();
    BlockedData(out, scene, environment); Concurrent(out, scene, executable, true); Concurrent(out, scene, secondExecutable, false);
    Clear(); Check(Empty() && Copies().empty(), "no private archive remains after all local leases are released"); environment.NoFallback();
    std::printf("PASS installation storage root=%ls other=%ls evidence=%ls\n", SharedTempRoot().c_str(), (secondExecutable.parent_path() / L"data" / L"mesh_bake_cache").c_str(), out.c_str()); return 0;
}
}
