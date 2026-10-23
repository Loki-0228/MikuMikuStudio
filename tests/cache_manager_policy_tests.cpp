// MAINTENANCE: regression-keep
// CPU/file-only policy checks. Every writable fixture is a newly claimed directory.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "../src/plugins/json.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#ifndef MMD_ORIGINAL_PROFILE
#error Pass the same MMD_ORIGINAL_PROFILE as mms_cache_policy.
#endif

// Same ordinary new/new[] seam as the repository's CPU history tests. The
// injected failure is current-thread-only and armed only around the API call.
namespace policy_allocation {
thread_local long remaining = -1;
thread_local unsigned hits = 0;
struct Fail {
    explicit Fail(long count) { remaining = count; hits = 0; }
    ~Fail() { remaining = -1; }
};
}
void* operator new(std::size_t size) {
    if (policy_allocation::remaining == 0) { ++policy_allocation::hits; throw std::bad_alloc(); }
    if (policy_allocation::remaining > 0) --policy_allocation::remaining;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace fs = std::filesystem;
namespace cache = mikumikustudio::cache_manager;
namespace settings = mikumikustudio::system_settings;
using mikumikustudio::plugins::Json;
using cache::Result;
namespace {
unsigned checks = 0;
constexpr char WorkerMarker[] = "MMS cache policy worker v1\n";
constexpr char InstallationMarker[] = "MMS cache policy test installation v1\n";
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : h(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
};
fs::path Executable() {
    wchar_t value[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, value, 32768);
    Check(n > 0 && n < 32768, "resolve actual EXE independently");
    return fs::path(value);
}
void Absolute(const fs::path& path) {
    Check(path.is_absolute() && path.has_filename() && path != path.root_path(), "require non-root absolute path");
    Check(path == path.lexically_normal(), "require normalized path without parent traversal");
}
void NewDirectory(const fs::path& path) {
    Absolute(path);
    Check(fs::is_directory(path.parent_path()), "new fixture parent already exists");
    Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "claim NEW directory exclusively");
}
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(bool(input), "open evidence file");
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Check(!input.bad(), "read evidence file");
    return bytes;
}
void NewFile(const fs::path& path, const std::string& bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(file.h != INVALID_HANDLE_VALUE, "create new owned fixture file");
    DWORD written = 0;
    Check(bytes.size() <= MAXDWORD && WriteFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size(), "write fixture bytes");
}
std::string Document(const std::string& section) {
    return "{\"schemaVersion\":1,\"sections\":{\"cacheManager\":" + section + "}}";
}
bool Equal(const cache::Configuration& a, const cache::Configuration& b) {
    return a.enabled == b.enabled && a.automaticCleanup == b.automaticCleanup &&
        a.limitMiB == b.limitMiB && a.recentProjects == b.recentProjects;
}
void Same(const cache::Snapshot& value, const cache::Snapshot& before, const char* why) {
    Check(value.IsValid() == before.IsValid() && Equal(value.Value(), before.Value()), why);
}
// Exercise the actual failed-call output, not a fresh copy of its sentinel.
// This checks private origin/presence/section CAS identity without inspecting
// representation. Fixtures passed here already contain all four known fields.
void StillUsable(const cache::Store& origin, const fs::path& file, const cache::Snapshot& failedOutput) {
    const auto bytes = Bytes(file);
    cache::Snapshot verified;
    Check(origin.Commit(failedOutput, failedOutput.Value(), verified) == Result::Ok,
        "failed output retains its original store identity and exact CAS section");
    Same(verified, failedOutput, "same-value verification publishes the same configuration");
    Check(Bytes(file) == bytes, "failed output verification preserves complete file including unknown fields");
}
struct Fixture {
    fs::path directory, file, root;
    cache::Store store;
    Fixture(const fs::path& parent, const std::wstring& name)
        : directory(parent / name), file(directory / L"settings.json"), root(directory / L"cache"), store(root, file) {
        NewDirectory(directory);
    }
};

void DefaultsAndBounds(const fs::path& root) {
    Fixture f(root, L"defaults");
    cache::Snapshot value;
    Check(!value.IsValid() && !value.Value().enabled && !value.Value().automaticCleanup, "invalid snapshot cannot default-enable");
    Check(f.store.Root() == f.root, "explicit store root");
    Check(f.store.Read(value) == Result::Ok && value.IsValid() && Equal(value.Value(), {}), "missing file defaults");
    Check(!fs::exists(f.file) && !fs::exists(f.root), "read defaults does not create settings or cache");
    Check(value.Value().LimitBytes() == UINT64_C(4294967296), "default 4 GiB uses 64-bit multiplication");
    struct MissingField { const char* object; cache::Configuration expected; };
    for (const auto& fixture : std::array<MissingField,6>{{
        {"{}", {true,true,4096,10}}, {"{\"enabled\":false}", {false,true,4096,10}},
        {"{\"automaticCleanup\":false}", {true,false,4096,10}}, {"{\"limitMiB\":64}", {true,true,64,10}},
        {"{\"recentProjects\":1}", {true,true,4096,1}},
        {"{\"enabled\":false,\"limitMiB\":102400,\"recentProjects\":100}", {false,true,102400,100}}}}) {
        Fixture item(root, L"missing-field-" + std::to_wstring(checks));
        NewFile(item.file, Document(fixture.object));
        cache::Snapshot got;
        Check(item.store.Read(got) == Result::Ok, "missing individual fields accepted");
        Check(Equal(got.Value(), fixture.expected), "each absent field independently defaults");
    }
    Fixture absent(root, L"absent-section");
    NewFile(absent.file, "{\"schemaVersion\":1,\"sections\":{\"unrelated\":{\"keep\":7}}}");
    Check(absent.store.Read(value) == Result::Ok && Equal(value.Value(), {}), "absent section defaults");
    for (const auto limit : {64u, 4096u, 102400u}) for (const auto recent : {1u, 10u, 100u}) {
        auto desired = cache::Configuration{false, false, limit, recent};
        Check(cache::Valid(desired), "inclusive configuration bounds");
        Check(absent.store.Commit(value, desired, value) == Result::Ok && Equal(value.Value(), desired), "same snapshot input-output alias commits");
        cache::Snapshot reread;
        Check(absent.store.Read(reread) == Result::Ok && Equal(reread.Value(), desired), "bounds persist actual settings");
        Check(reread.Value().LimitBytes() == std::uint64_t(limit) * UINT64_C(1048576), "all bounds preserve 64-bit bytes");
    }
    for (const auto desired : {cache::Configuration{true,true,63,10}, {true,true,102401,10},
        {true,true,4096,0}, {true,true,4096,101}, {true,true,UINT32_MAX,10}, {true,true,4096,UINT32_MAX}}) {
        const auto before = value;
        const auto bytes = Bytes(absent.file);
        Check(!cache::Valid(desired) && absent.store.Commit(value, desired, value) == Result::Invalid, "reject out-of-range desired config");
        Same(value, before, "invalid desired preserves aliased snapshot");
        Check(Bytes(absent.file) == bytes, "invalid desired preserves file bytes");
        StillUsable(absent.store, absent.file, value);
    }
}

void InvalidInputs(const fs::path& root) {
    Fixture good(root, L"output-sentinel");
    NewFile(good.file, Document("{\"future\":{\"array\":[null,1e2,\"keep\"]}}"));
    cache::Snapshot sentinel;
    Check(good.store.Read(sentinel) == Result::Ok, "prepare valid output sentinel");
    auto custom = cache::Configuration{false,false,128,3};
    Check(good.store.Commit(sentinel, custom, sentinel) == Result::Ok, "prepare distinctive output sentinel");
    const std::array<const char*, 4> fields{"enabled", "automaticCleanup", "limitMiB", "recentProjects"};
    for (const char* field : fields) {
        std::vector<std::string> bad{"null", "\"true\"", "[]", "{}", "-1", "1.5", "1e2", "18446744073709551616"};
        if (std::string(field) == "enabled" || std::string(field) == "automaticCleanup") {
            bad.push_back("0"); bad.push_back("1");
        } else { bad.push_back("true"); bad.push_back("false"); bad.push_back("0"); }
        if (std::string(field) == "limitMiB") { bad.push_back("63"); bad.push_back("102401"); }
        if (std::string(field) == "recentProjects") bad.push_back("101");
        for (const auto& token : bad) {
            Fixture f(root, L"bad-field-" + std::to_wstring(checks));
            const auto bytes = Document("{\"" + std::string(field) + "\":" + token + "}");
            cache::Snapshot expected;
            Check(f.store.Read(expected) == Result::Ok, "prepare same-store pre-corruption snapshot");
            NewFile(f.file, bytes);
            auto output = sentinel;
            Check(f.store.Read(output) == Result::Invalid, "present invalid known field rejects read");
            Same(output, sentinel, "invalid read preserves valid output");
            StillUsable(good.store, good.file, output);
            Check(f.store.Commit(expected, custom, output) == Result::Conflict,
                "changed on-disk invalid field rejects old snapshot through exact-section CAS");
            Same(output, sentinel, "invalid commit preserves output");
            StillUsable(good.store, good.file, output);
            Check(Bytes(f.file) == bytes, "invalid known field is never overwritten");
            cache::Snapshot invalid;
            Check(f.store.Read(invalid) == Result::Invalid && !invalid.IsValid() && !invalid.Value().enabled,
                "invalid data never silently enables defaults");
        }
    }
    for (const auto& bytes : std::vector<std::string>{"{", "[]", "null", "{}",
        "{\"schemaVersion\":2,\"sections\":{}}", "{\"schemaVersion\":null,\"sections\":{}}",
        "{\"schemaVersion\":1.0,\"sections\":{}}", "{\"schemaVersion\":1e0,\"sections\":{}}",
        "{\"schemaVersion\":1,\"sections\":[]}", "{\"schemaVersion\":1,\"sections\":{\"cacheManager\":null}}",
        "{\"schemaVersion\":1,\"sections\":{\"other\":[]}}",
        "{\"schemaVersion\":1,\"sections\":{},\"sections\":{}}", std::string(1024*1024+1, ' ')}) {
        Fixture f(root, L"bad-document-" + std::to_wstring(checks));
        cache::Snapshot expected;
        Check(f.store.Read(expected) == Result::Ok, "prepare before corrupt document");
        NewFile(f.file, bytes);
        auto output = sentinel;
        Check(f.store.Read(output) == Result::Invalid, "bad JSON or schema rejects read");
        Same(output, sentinel, "bad document read preserves output");
        StillUsable(good.store, good.file, output);
        Check(f.store.Commit(expected, custom, output) == Result::Invalid, "bad JSON or schema rejects commit");
        Same(output, sentinel, "bad document preserves output");
        StillUsable(good.store, good.file, output);
        Check(Bytes(f.file) == bytes, "bad document preserves file bytes");
    }
}

void SnapshotAndUnknown(const fs::path& root) {
    Fixture f(root, L"snapshot");
    NewFile(f.file, "{\"schemaVersion\":1,\"futureRoot\":{\"n\":1e2,\"unicode\":\"\\u4e2d\\u6587\\ud83d\\ude00\"},"
        "\"sections\":{\"cacheManager\":{\"enabled\":false,\"future\":{\"array\":[null,true,1e2,{\"x\":\"\\u96ea\"}]}},"
        "\"unrelated\":{\"array\":[1,2,3]}}}");
    const auto initial = Json::Parse(Bytes(f.file));
    cache::Snapshot first, output;
    Check(f.store.Read(first) == Result::Ok, "read unknown-field snapshot");
    const auto desired = cache::Configuration{false,false,8192,20};
    const auto beforeInvalid = Bytes(f.file);
    Check(f.store.Commit(output, desired, output) == Result::Invalid && !output.IsValid(), "invalid snapshot rejects commit");
    Check(Bytes(f.file) == beforeInvalid, "invalid snapshot cannot write");
    Fixture other(root, L"other-store");
    Check(other.store.Commit(first, desired, output) == Result::Invalid && !fs::exists(other.file), "cross-settings snapshot rejects");
    cache::Store otherRoot(other.root, f.file);
    Check(otherRoot.Commit(first, desired, output) == Result::Invalid && Bytes(f.file) == beforeInvalid, "cross-cache-root snapshot rejects");
    Check(settings::Store(f.file).WriteSection("unrelated", "{\"changed\":77}") == settings::Result::Ok,
        "unrelated concurrent section change");
    Check(f.store.Commit(first, desired, output) == Result::Ok, "unrelated section does not expire cache CAS");
    const auto document = Json::Parse(Bytes(f.file));
    Check(document.At("futureRoot").At("n").text == "1e2" &&
        document.At("futureRoot").At("unicode").text == initial.At("futureRoot").At("unicode").text,
        "unknown root number spelling and Unicode preserved");
    const auto& future = document.At("sections").At("cacheManager").At("future").At("array").array;
    Check(future.size() == 4 && future[0].kind == Json::Null && future[1].text == "true" && future[2].text == "1e2" &&
        future[3].At("x").text == initial.At("sections").At("cacheManager").At("future").At("array").array[3].At("x").text,
        "unknown nested arrays and Unicode preserved");
    Check(document.At("sections").At("unrelated").At("changed").UInt() == 77 &&
        document.At("sections").At("unrelated").At("array").array.size() == 3, "unrelated complete section preserved");
    const auto current = output;
    const auto bytes = Bytes(f.file);
    Check(f.store.Commit(first, {}, output) == Result::Conflict, "stale same-section CAS rejected");
    Same(output, current, "stale CAS preserves output");
    Check(Bytes(f.file) == bytes, "stale CAS preserves bytes");
    StillUsable(f.store, f.file, output);
    Check(f.store.Commit(first, {}, first) == Result::Conflict && !first.Value().enabled,
        "stale aliased CAS keeps old expected snapshot");
    Check(f.store.Commit(output, desired, output) == Result::Ok, "same-output alias remains usable after conflict");
    cache::Store relativeRoot(L"relative", f.file), relativeFile(f.root, L"relative.json");
    Check(relativeRoot.Read(output) == Result::Invalid && relativeFile.Read(output) == Result::Invalid, "relative locations rejected");
    Same(output, current, "invalid paths preserve snapshot");
    StillUsable(f.store, f.file, output);
}

void FailureChecks(const fs::path& root) {
    Fixture f(root, L"io-failures");
    NewFile(f.file, Document("{\"future\":{\"array\":[null,1e2,\"keep\"]}}"));
    cache::Snapshot value;
    Check(f.store.Read(value) == Result::Ok, "read failure fixture");
    const cache::Configuration disabled{false,false,128,4};
    Check(f.store.Commit(value, disabled, value) == Result::Ok, "save disabled failure baseline");
    const auto before = value;
    const auto bytes = Bytes(f.file);
    const auto lockPath = fs::path(f.file.wstring() + L".lock");
    auto busyRead = value, busyCommit = value;
    {
        Handle lock(CreateFileW(lockPath.c_str(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        OVERLAPPED overlap{};
        Check(lock.h != INVALID_HANDLE_VALUE && LockFileEx(lock.h, LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,
            0, 1, 0, &overlap), "hold actual exclusive byte lock");
        Check(f.store.Read(busyRead) == Result::Busy && f.store.Commit(before, {}, busyCommit) == Result::Busy, "real contention reports Busy for read and commit");
        Same(busyRead, before, "Busy read preserves disabled snapshot");
        Same(busyCommit, before, "Busy commit preserves disabled snapshot");
        Check(UnlockFileEx(lock.h, 0, 1, 0, &overlap) != FALSE, "release owned byte lock");
    }
    StillUsable(f.store, f.file, busyRead);
    StillUsable(f.store, f.file, busyCommit);
    auto ioRead = value, ioCommit = value;
    {
        Handle deny(CreateFileW(f.file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        Check(deny.h != INVALID_HANDLE_VALUE, "open exclusive read-denial fixture");
        Check(f.store.Read(ioRead) == Result::Io && f.store.Commit(before, {}, ioCommit) == Result::Io, "file sharing denial reports Io");
        Same(ioRead, before, "file denial preserves read snapshot");
        Same(ioCommit, before, "file denial preserves commit snapshot");
    }
    StillUsable(f.store, f.file, ioRead);
    StillUsable(f.store, f.file, ioCommit);
    {
        Handle noReplace(CreateFileW(f.file.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        Check(noReplace.h != INVALID_HANDLE_VALUE, "hold readable file without delete sharing");
        Check(f.store.Read(value) == Result::Ok && f.store.Commit(value, {}, value) == Result::Io,
            "load succeeds but real atomic publication fails");
        Same(value, before, "publish failure does not default-enable output");
    }
    StillUsable(f.store, f.file, value);
    Check(Bytes(f.file) == bytes, "all failures preserve original bytes");
    for (const auto& entry : fs::directory_iterator(f.directory))
        Check(entry.path().filename().wstring().find(L".tmp-") == std::wstring::npos, "publication failure cleans only its own temporary");
    const auto blocked = root / L"blocked-parent";
    cache::Store cannotCreate(blocked / L"cache", blocked / L"settings.json");
    cache::Snapshot missing;
    Check(cannotCreate.Read(missing) == Result::Ok, "capture missing parent before obstruction");
    NewFile(blocked, "owned directory obstruction\n");
    const auto missingBefore = missing;
    Check(cannotCreate.Commit(missing, disabled, missing) == Result::Io, "directory creation failure reports Io");
    Same(missing, missingBefore, "directory obstruction preserves output");
    Check(Bytes(blocked) == "owned directory obstruction\n", "no overwrite of parent obstruction");
}

void AllocationChecks(const fs::path& root) {
    bool caught = false;
    void* (*volatile allocate)(std::size_t) = &::operator new;
    {
        policy_allocation::Fail fail(0);
        try { void* memory = allocate(64); ::operator delete(memory); }
        catch (const std::bad_alloc&) { caught = true; }
    }
    Check(caught && policy_allocation::hits == 1, "calibrate current-thread allocation failure hook");
    Fixture seed(root, L"allocation-sentinel");
    NewFile(seed.file, Document("{\"future\":{\"array\":[null,1e2,\"keep\"]}}"));
    cache::Snapshot sentinel;
    Check(seed.store.Read(sentinel) == Result::Ok &&
        seed.store.Commit(sentinel, {false,false,512,6}, sentinel) == Result::Ok, "prepare allocation output sentinel");
    for (const bool invalidInteger : {false, true}) {
        Fixture f(root, invalidInteger ? L"allocation-invalid-integer" : L"allocation-valid");
        const auto bytes = Document(invalidInteger ?
            "{\"enabled\":true,\"automaticCleanup\":true,\"limitMiB\":4e3,\"recentProjects\":10}" :
            "{\"enabled\":true,\"automaticCleanup\":true,\"limitMiB\":4096,\"recentProjects\":10}");
        NewFile(f.file, bytes);
        bool completed = false;
        unsigned failures = 0;
        for (long failAfter = 0; failAfter < 4096; ++failAfter) {
            auto output = sentinel;
            Result result;
            {
                policy_allocation::Fail fail(failAfter);
                result = f.store.Read(output);
            }
            if (policy_allocation::hits) {
                ++failures;
                Check(result == Result::Io, "every triggered read allocation failure reports Io, not Invalid or termination");
                Same(output, sentinel, "allocation failure preserves read output");
                StillUsable(seed.store, seed.file, output);
            } else {
                Check(result == (invalidInteger ? Result::Invalid : Result::Ok), "unfaulted read retains semantic result");
                if (invalidInteger) {
                    Same(output, sentinel, "invalid integer preserves output without allocation fault");
                    StillUsable(seed.store, seed.file, output);
                }
                else Check(output.IsValid() && Equal(output.Value(), {}), "unfaulted read publishes complete policy");
                completed = true; break;
            }
        }
        Check(completed && failures > 4, "read scan reaches all ordinary allocations through final decode");
        Check(Bytes(f.file) == bytes, "allocation read scan never writes settings");
    }
    Fixture f(root, L"allocation-commit");
    NewFile(f.file, Document("{\"future\":{\"array\":[null,1e2,\"keep\"]}}"));
    cache::Snapshot expected;
    Check(f.store.Read(expected) == Result::Ok &&
        f.store.Commit(expected, {false,false,256,5}, expected) == Result::Ok, "prepare existing commit allocation fixture");
    const auto before = Bytes(f.file);
    const cache::Configuration desired{true,true,8192,8};
    bool completed = false;
    unsigned failures = 0;
    for (long failAfter = 0; failAfter < 4096; ++failAfter) {
        auto output = expected; // Exercise same expected/output identity, copying before injection.
        Result result;
        {
            policy_allocation::Fail fail(failAfter);
            result = f.store.Commit(output, desired, output);
        }
        if (policy_allocation::hits) {
            ++failures;
            Check(result == Result::Io, "triggered commit allocation failure reports Io");
            Same(output, expected, "commit allocation failure preserves aliased snapshot");
            Check(Bytes(f.file) == before, "commit allocation failure occurs before file publication");
            StillUsable(f.store, f.file, output);
        } else {
            Check(result == Result::Ok && Equal(output.Value(), desired), "unfaulted commit publishes output and file");
            cache::Snapshot reread;
            Check(f.store.Read(reread) == Result::Ok && Equal(reread.Value(), desired), "successful scan endpoint really persisted");
            completed = true; break;
        }
    }
    Check(completed && failures > 4, "commit scan reaches final allocation before publication");
}

int Worker(const fs::path& root, unsigned id) {
    Absolute(root);
    Check(id < 3 && Bytes(root / L"worker-marker.txt") == WorkerMarker, "worker only accesses parent-owned fixture");
    const auto start = GetTickCount64();
    while (!fs::exists(root / L"go")) {
        Check(GetTickCount64() - start < 30000, "worker start barrier bounded"); Sleep(2);
    }
    const auto file = root / L"settings.json";
    if (id == 2) {
        for (unsigned i = 1; i <= 40; ++i)
            Check(settings::Store(file).WriteSection("parallelUnrelated", "{\"count\":" + std::to_string(i) + "}") == settings::Result::Ok,
                "worker writes unrelated section");
    } else {
        cache::Store store(root / L"cache", file);
        for (unsigned i = 0; i < 20; ++i) {
            bool completed = false;
            for (unsigned attempt = 0; attempt < 1000; ++attempt) {
                cache::Snapshot snapshot;
                const auto read = store.Read(snapshot);
                if (read == Result::Busy) { Sleep(1); continue; }
                Check(read == Result::Ok, "worker reads actual policy");
                auto desired = snapshot.Value(); ++desired.recentProjects;
                const auto committed = store.Commit(snapshot, desired, snapshot);
                if (committed == Result::Conflict || committed == Result::Busy) { Sleep(1); continue; }
                Check(committed == Result::Ok, "worker CAS commit succeeds"); completed = true; break;
            }
            Check(completed, "bounded CAS retry succeeded");
        }
    }
    NewFile(root / (L"worker-" + std::to_wstring(id) + L".done"), "completed\n");
    return 0;
}
struct Child {
    PROCESS_INFORMATION process{};
    bool reaped = false;
    ~Child() {
        if (process.hProcess) {
            if (!reaped && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
                TerminateProcess(process.hProcess, 125); WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
};
void Concurrent(const fs::path& root) {
    Fixture f(root, L"processes");
    NewFile(f.directory / L"worker-marker.txt", WorkerMarker);
    NewFile(f.file, Document("{\"recentProjects\":10,\"enabled\":false,\"future\":[1,\"keep\"]}"));
    const auto executable = Executable();
    std::array<Child,3> children;
    bool launches = true;
    std::string exits;
    for (unsigned i = 0; i < children.size(); ++i) {
        auto command = L"\"" + executable.wstring() + L"\" --worker \"" + f.directory.wstring() + L"\" " + std::to_wstring(i);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        const bool launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, f.directory.c_str(), &startup, &children[i].process) != FALSE;
        const DWORD error = launched ? ERROR_SUCCESS : GetLastError();
        launches = launches && launched;
        exits += "worker=" + std::to_string(i) + " launch=" + std::to_string(launched) + " error=" + std::to_string(error) + "\n";
    }
    NewFile(f.directory / L"go", "go\n");
    bool all = launches;
    for (unsigned i = 0; i < children.size(); ++i) {
        auto& child = children[i];
        if (!child.process.hProcess) continue;
        const auto waited = WaitForSingleObject(child.process.hProcess, 60000);
        if (waited != WAIT_OBJECT_0) { TerminateProcess(child.process.hProcess, 124); WaitForSingleObject(child.process.hProcess, 5000); }
        DWORD code = STILL_ACTIVE;
        const bool queried = GetExitCodeProcess(child.process.hProcess, &code) != FALSE;
        child.reaped = queried && code != STILL_ACTIVE;
        all = all && waited == WAIT_OBJECT_0 && queried && code == 0;
        exits += "worker=" + std::to_string(i) + " wait=" + std::to_string(waited) + " exit=" + std::to_string(code) + "\n";
    }
    NewFile(f.directory / L"worker-exit-codes.txt", exits);
    Check(all, "all real subprocess exit codes retained and zero");
    cache::Snapshot final;
    Check(f.store.Read(final) == Result::Ok && final.Value().recentProjects == 50 && !final.Value().enabled,
        "two processes CAS increments never lose updates");
    const auto document = Json::Parse(Bytes(f.file));
    Check(document.At("sections").At("parallelUnrelated").At("count").UInt() == 40 &&
        document.At("sections").At("cacheManager").At("future").array[1].text == "keep",
        "concurrent unrelated writer and unknown cache fields survive");
    for (unsigned i = 0; i < 3; ++i) Check(Bytes(f.directory / (L"worker-"+std::to_wstring(i)+L".done")) == "completed\n", "worker completed all iterations");
}

struct Environment {
    struct Entry { std::wstring name, value; bool present; };
    std::vector<Entry> entries;
    fs::path cwd = fs::current_path();
    void Set(const wchar_t* name, const fs::path& path) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        std::wstring old(size, L'\0');
        if (size) { GetEnvironmentVariableW(name, old.data(), size); old.resize(size-1); }
        entries.push_back({name, old, size != 0});
        Check(SetEnvironmentVariableW(name, path.c_str()) != FALSE, "set owned environment decoy");
    }
    ~Environment() {
        SetCurrentDirectoryW(cwd.c_str());
        for (auto i = entries.rbegin(); i != entries.rend(); ++i)
            SetEnvironmentVariableW(i->name.c_str(), i->present ? i->value.c_str() : nullptr);
    }
};
void Installation(const fs::path& output) {
    const auto installation = Executable().parent_path();
    Check(Bytes(installation / L"cache-policy-test-installation.txt") == InstallationMarker, "explicit isolated-installation marker");
    Check(!fs::exists(installation / L"data"), "installation starts without data");
    Check(!fs::exists(installation / L"PLUGINS") || fs::is_empty(installation / L"PLUGINS"), "installation has no plugin payload");
    NewDirectory(output);
    const auto decoy = output / L"decoy"; NewDirectory(decoy);
    const auto cwd = output / L"cwd"; NewDirectory(cwd);
    NewFile(decoy / L"settings.json", Document("{\"enabled\":false,\"limitMiB\":64,\"recentProjects\":1}"));
    NewFile(cwd / L"settings.json", "cwd must not be used\n");
    NewFile(decoy / L"cache.ini", "legacy decoy must not be used\n");
    NewFile(output / L"project.pmm", "project sentinel\n");
    NewFile(output / L"history.bin", "history sentinel\n");
    const auto decoyBytes = Bytes(decoy / L"settings.json");
    Environment environment;
    for (const wchar_t* name : {L"MMS_CACHE_MANAGER_DIR", L"LOCALAPPDATA", L"APPDATA", L"USERPROFILE", L"TEMP", L"TMP"})
        environment.Set(name, decoy);
    Check(SetCurrentDirectoryW(cwd.c_str()) != FALSE, "change cwd to own decoy");
    cache::Snapshot snapshot;
#if MMD_ORIGINAL_PROFILE
    Check(cache::Root().empty(), "original production root is disabled");
    Check(cache::ReadCurrent(snapshot) == Result::Disabled && !snapshot.IsValid(), "original ReadCurrent disabled");
    Check(cache::Configure(snapshot, {}, snapshot) == Result::Disabled && !snapshot.IsValid(), "original Configure disabled");
    Check(!cache::Enabled() && !fs::exists(installation / L"data"), "original wrappers do not write data");
#else
    const auto expectedRoot = installation / L"data" / L"cache";
    const auto expectedSettings = installation / L"data" / L"settings.json";
    Check(cache::Root() == expectedRoot, "production root uses own EXE directory despite env and cwd");
    Check(cache::ReadCurrent(snapshot) == Result::Ok && Equal(snapshot.Value(), {}) && cache::Enabled(), "fresh production defaults ignore decoys");
    Check(!fs::exists(expectedSettings) && !fs::exists(expectedRoot), "production read is not cache creation");
    const cache::Configuration custom{false,false,8192,7};
    Check(cache::Configure(snapshot, custom, snapshot) == Result::Ok && !cache::Enabled(), "production Configure writes disabled policy");
    Check(fs::is_regular_file(expectedSettings) && !fs::exists(expectedRoot), "only per-installation settings persisted");
    cache::Snapshot reread;
    Check(cache::ReadCurrent(reread) == Result::Ok && Equal(reread.Value(), custom), "production settings round trip");
    NewDirectory(expectedRoot);
    NewFile(expectedRoot / L"cache-sentinel.bin", "do not scan or evict\n");
    auto enabled = custom; enabled.enabled = true; enabled.automaticCleanup = true;
    Check(cache::Configure(snapshot, enabled, snapshot) == Result::Ok && cache::Enabled(), "configuration enables policy without performing cleanup");
    const auto fileBefore = Bytes(expectedSettings);
    const cache::Store productionStore(expectedRoot, expectedSettings);
    auto ioRead = snapshot, ioCommit = snapshot;
    {
        Handle deny(CreateFileW(expectedSettings.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        Check(deny.h != INVALID_HANDLE_VALUE, "deny production settings read");
        Check(!cache::Enabled(), "production read failure must not default-enable");
        Check(cache::ReadCurrent(ioRead) == Result::Io && cache::Configure(snapshot, custom, ioCommit) == Result::Io,
            "production wrappers expose settings IO failure");
        Same(ioRead, snapshot, "production failed read preserves output");
        Same(ioCommit, snapshot, "production failed commit preserves output");
    }
    StillUsable(productionStore, expectedSettings, ioRead);
    StillUsable(productionStore, expectedSettings, ioCommit);
    auto busyRead = snapshot, busyCommit = snapshot;
    {
        const auto lockPath = fs::path(expectedSettings.wstring() + L".lock");
        Handle lock(CreateFileW(lockPath.c_str(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        OVERLAPPED overlap{};
        Check(lock.h != INVALID_HANDLE_VALUE && LockFileEx(lock.h, LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,
            0, 1, 0, &overlap), "hold actual production settings byte lock");
        Check(!cache::Enabled(), "production Busy must not default-enable");
        Check(cache::ReadCurrent(busyRead) == Result::Busy && cache::Configure(snapshot, custom, busyCommit) == Result::Busy,
            "production wrappers expose Busy from actual lock");
        Same(busyRead, snapshot, "production Busy read preserves output");
        Same(busyCommit, snapshot, "production Busy commit preserves output");
        Check(UnlockFileEx(lock.h, 0, 1, 0, &overlap) != FALSE, "release production byte lock");
    }
    StillUsable(productionStore, expectedSettings, busyRead);
    StillUsable(productionStore, expectedSettings, busyCommit);
    Check(cache::Enabled(), "production policy recovers after lock release");

    // Both names belong to the freshly marked installation. No replace flag,
    // recursive cleanup or existing user-file overwrite is used. On failure,
    // leave the valid backup and malformed bytes as evidence.
    const auto saved = expectedSettings.parent_path() / L"settings-before-invalid.json";
    const auto rejected = expectedSettings.parent_path() / L"settings-rejected-invalid.json";
    Check(MoveFileExW(expectedSettings.c_str(), saved.c_str(), 0) != FALSE, "preserve owned production settings before corruption");
    NewFile(expectedSettings, "{");
    auto invalidRead = snapshot, invalidCommit = snapshot;
    Check(!cache::Enabled(), "production Invalid must not default-enable");
    Check(cache::ReadCurrent(invalidRead) == Result::Invalid && cache::Configure(snapshot, custom, invalidCommit) == Result::Invalid,
        "production wrappers reject actual malformed settings");
    Same(invalidRead, snapshot, "production invalid read preserves output");
    Same(invalidCommit, snapshot, "production invalid commit preserves output");
    Check(Bytes(expectedSettings) == "{" && Bytes(saved) == fileBefore, "invalid production request preserves malformed input and valid backup");
    Check(MoveFileExW(expectedSettings.c_str(), rejected.c_str(), 0) != FALSE &&
        MoveFileExW(saved.c_str(), expectedSettings.c_str(), 0) != FALSE, "retain malformed evidence and restore valid production settings");
    StillUsable(productionStore, expectedSettings, invalidRead);
    StillUsable(productionStore, expectedSettings, invalidCommit);
    Check(cache::Enabled(), "production policy recovers after valid settings restoration");
    Check(Bytes(expectedSettings) == fileBefore && Bytes(expectedRoot / L"cache-sentinel.bin") == "do not scan or evict\n",
        "policy changes and failures do not alter cache sentinel");
#endif
    Check(Bytes(decoy / L"settings.json") == decoyBytes && Bytes(decoy / L"cache.ini") == "legacy decoy must not be used\n" &&
        Bytes(cwd / L"settings.json") == "cwd must not be used\n", "environment and cwd decoys unchanged");
    Check(std::distance(fs::directory_iterator(decoy), fs::directory_iterator()) == 2 &&
        std::distance(fs::directory_iterator(cwd), fs::directory_iterator()) == 1, "no fallback files in decoy locations");
    Check(!fs::exists(installation / L"Cache") && !fs::exists(installation / L"cache.ini"), "no legacy cache root or ini written");
    Check(Bytes(output / L"project.pmm") == "project sentinel\n" && Bytes(output / L"history.bin") == "history sentinel\n",
        "configuration leaves project and history untouched");
    NewFile(output / L"installation-result.txt", "profile=" + std::string(MMD_ORIGINAL_PROFILE ? "original" : "full") +
        "\nexe=" + mikumikustudio::plugins::Utf8(Executable().wstring()) + "\nchecks=" + std::to_string(checks) + "\n");
}
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--worker") {
            const std::wstring id = argv[3];
            Check(id == L"0" || id == L"1" || id == L"2", "known worker id");
            return Worker(fs::path(argv[2]), unsigned(id[0] - L'0'));
        }
        Check(argc == 3, "usage: --store NEW_ABSOLUTE_DIR or --installation NEW_ABSOLUTE_DIR");
        const fs::path root(argv[2]); Absolute(root);
        if (std::wstring(argv[1]) == L"--installation") Installation(root);
        else {
            Check(std::wstring(argv[1]) == L"--store", "known explicit mode");
            NewDirectory(root);
            DefaultsAndBounds(root); InvalidInputs(root); SnapshotAndUnknown(root); FailureChecks(root); AllocationChecks(root); Concurrent(root);
            NewFile(root / L"store-result.txt", "checks=" + std::to_string(checks) + "\n");
        }
        std::printf("PASS cache policy checks=%u evidence=%ls\n", checks, root.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL cache policy checks=%u: %s\n", checks, error.what()); return 1;
    } catch (...) { std::fprintf(stderr, "FAIL cache policy unexpected exception\n"); return 2; }
}
