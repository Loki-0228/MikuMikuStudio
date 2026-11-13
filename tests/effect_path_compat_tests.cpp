// Execute the same path/IAT bodies without loading or distributing MME.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
namespace {
HANDLE faultFile = INVALID_HANDLE_VALUE;
bool failRestore = false, failRead = false;
unsigned partialReads = 0, restoreFailures = 0;
BOOL WINAPI TestAssignmentSetPointer(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
BOOL WINAPI TestAssignmentRead(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
}
#define SetFilePointerEx TestAssignmentSetPointer
#define ReadFile TestAssignmentRead
#include "../src/exports/effect_paths.cpp"
#undef ReadFile
#undef SetFilePointerEx
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>

namespace mikumikustudio::plugins {
const LegacyMmeStartupStatus& LegacyMmeStatus() noexcept {
    static const LegacyMmeStartupStatus status{};
    return status;
}
}
namespace mikumikustudio::effect_compile_cache {
bool TryCreateEffect(void*, IDirect3DDevice9*, LPCWSTR, const void*, const void*,
                     DWORD, void*, void**, void**, HRESULT&) noexcept { return false; }
}

namespace {
thread_local bool failAllocation = false;
unsigned traces = 0;
wchar_t tracedPath[32768]{}, compilerPath[32768]{};
char compiledBytes[64]{};
DWORD compiledLength = 0;
BOOL WINAPI TestAssignmentSetPointer(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD origin) {
    if (file == faultFile && failRestore && origin == FILE_BEGIN && distance.QuadPart == 7) {
        ++restoreFailures; failRestore = false; SetLastError(ERROR_SEEK); return FALSE;
    }
    return SetFilePointerEx(file, distance, position, origin);
}
BOOL WINAPI TestAssignmentRead(HANDLE file, LPVOID bytes, DWORD count, LPDWORD read, LPOVERLAPPED overlapped) {
    if (file == faultFile && failRead) {
        ++partialReads; failRead = false;
        const BOOL result = ReadFile(file, bytes, count ? 1 : 0, read, overlapped);
        if (!result) return result;
        SetLastError(ERROR_CRC); return FALSE;
    }
    return ReadFile(file, bytes, count, read, overlapped);
}
}
void* operator new(std::size_t size) {
    if (failAllocation) throw std::bad_alloc();
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace mikumikustudio::runtime_log {
void Write(const char*, ...) noexcept { ++traces; SetLastError(8811); }
void WritePath(const char*, const wchar_t* path) noexcept {
    ++traces; wcscpy_s(tracedPath, path ? path : L""); SetLastError(8812);
}
}
namespace {
using namespace mikumikustudio;
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s (error=%lu)\n", message, GetLastError()); std::exit(1); }
}
void Write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary); out << text;
    Check(bool(out), "create owned path fixture");
}
std::string Bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}};
}
std::string NativeBytes(const fs::path& file) {
    std::string value;
    Check(text_encoding::EncodeExact(file.c_str(), GetACP(), value), "encode exact native path for API fixture");
    return value;
}
std::string Read(const char* path) {
    HANDLE file = OpenEffectFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open existing effect through compatibility API");
    std::array<char, 64> bytes{}; DWORD count = 0;
    Check(ReadFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr) != FALSE, "read actual resolved file");
    CloseHandle(file); return {bytes.data(), count};
}
void ProjectSuffixPaths(const fs::path& root) {
    for (const auto* suffix : {L".pmm", L".pmm1", L".PmM1111", L".pmm-recovery"}) {
        const auto project = root / (std::wstring(L"suffix") + suffix);
        const auto expected = root / (L"suffix.emm" + std::wstring(suffix).substr(4));
        Check(pmm_paths::IsProject(project) && pmm_paths::Effect(project) == expected,
            "PMM filename suffix pairs with the same EMM suffix without selecting a format version");
    }
    for (const auto* suffix : {L".emm", L".emm1", L".EmM1111", L".emm-recovery"}) {
        const auto effect = root / (std::wstring(L"explicit") + suffix);
        Check(pmm_paths::IsEffect(effect) && !pmm_paths::IsProject(effect) && pmm_paths::Effect(effect) == effect,
            "an explicitly chosen EMM retains its exact suffix and spelling");
    }
    Check(!pmm_paths::IsProject(root / L"folder.pmm" / L"picture.png") &&
        !pmm_paths::IsProject(root / L"scene.pmm1.txt") && !pmm_paths::IsEffect(root / L"scene.emm1.fx"),
        "only the final extension routes project and assignment files");
}
void ProjectNotificationAliases(const fs::path& root) {
    const auto folder = root / (L"notification-alias-" + std::to_wstring(GetCurrentProcessId()));
    const auto project = folder / L"scene.pmm1";
    const auto effect = pmm_paths::Effect(project);
    const auto baseEffect = folder / L"scene.emm";
    const auto local = folder / L"notification-effects" / L"assigned.fx";
    const auto legacy = root / L"notification-effects" / L"assigned.fx";
    const std::string assignment = "[Effect]\nPmd1=notification-effects/assigned.fx\n";
    Write(project, "PMM suffix routing fixture"); Write(effect, assignment);
    Write(baseEffect, "base-effect-must-stay"); Write(local, "version-project"); Write(legacy, "version-exe");
    const auto before = projectAliases.size();
    const auto baseProject = folder / L"scene.pmm";
    SetLastError(6611);
    Check(EffectProjectNotificationPath(baseProject.c_str()) == baseProject.wstring() && GetLastError() == 6611 &&
        projectAliases.size() == before, "plain PMM notifications use their own path without consuming an alias");
    SetLastError(6612);
    Check(EffectProjectNotificationPath(nullptr).empty() && EffectProjectNotificationPath(L"").empty() && GetLastError() == 6612,
        "empty notification input neither registers an alias nor changes last error");
    SetLastError(6613);
    const fs::path notification = EffectProjectNotificationPath(project.c_str());
    const DWORD preparedError = GetLastError();
    auto virtualEffect = notification; virtualEffect.replace_extension(L".emm");
    Check(!notification.empty() && notification.extension() == L".pmm" && notification.parent_path() == folder &&
        projectAliases.size() == before + 1 && preparedError == 6613 && !fs::exists(notification) && !fs::exists(virtualEffect),
        "versioned project registers one absent virtual PMM and EMM without materializing either file");
    const auto alternate = folder / L"." / project.filename();
    SetLastError(6614);
    Check(EffectProjectNotificationPath(alternate.c_str()) == notification.wstring() && GetLastError() == 6614 &&
        projectAliases.size() == before + 1, "repeated normalized project notifications reuse the same stable alias");
    SetLastError(6615);
    Check(EffectProjectNotificationPath(effect.c_str()) == effect.wstring() && GetLastError() == 6615 &&
        projectAliases.size() == before + 1, "explicit versioned EMM notification remains its exact file");
    const auto alias = NativeBytes(virtualEffect), expanded = NativeBytes(legacy);
    PrepareEffectProjectPaths(project.c_str());
    Check(Read(expanded.c_str()) == "version-project", "PMM1 preparation uses EMM1 relative assignments");
    PrepareEffectProjectPaths(effect.c_str());
    Check(Read(expanded.c_str()) == "version-project", "explicit EMM1 preparation keeps its own assignment suffix");
    PrepareEffectProjectPaths(nullptr);
    Check(Read(expanded.c_str()) == "version-exe", "workspace reset still retires only relative assignments");
    Check(Read(alias.c_str()) == assignment && Read(expanded.c_str()) == "version-project",
        "queued notification alias survives reset and parsing the actual EMM1 restores its relative effects");
    Check(EffectFileAttributes(alias.c_str()) == GetFileAttributesW(effect.c_str()),
        "registered virtual EMM attributes describe its exact retained file");
    HANDLE file = OpenEffectFile(alias.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "mapped EMM1 can be opened exclusively for a same-handle cursor check");
    LARGE_INTEGER zero{}, seven{}, position{}; seven.QuadPart = 7;
    Check(SetFilePointerEx(file, zero, &position, FILE_CURRENT) && position.QuadPart == 0 &&
        SetFilePointerEx(file, seven, nullptr, FILE_BEGIN), "mapped EMM1 begins at byte zero and permits a nonzero cursor");
    SetLastError(6616);
    Check(TrackAssignmentRead(file, true, FILE_ATTRIBUTE_NORMAL) == file && GetLastError() == 6616 &&
        SetFilePointerEx(file, zero, &position, FILE_CURRENT) && position.QuadPart == 7,
        "versioned same-handle parsing preserves caller cursor and last error");
    CloseHandle(file);
    std::array<wchar_t, 32768> converted{};
    SetLastError(6617);
    Check(DecodeEffectPath(CP_ACP, 0, alias.c_str(), -1, converted.data(), int(converted.size())) > 0 &&
        fs::path(converted.data()) == virtualEffect && GetLastError() == 6617,
        "generic byte conversion preserves the virtual spelling rather than applying a write alias");
    file = CreateFileW(converted.data(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND && Bytes(effect) == assignment,
        "ordinary wide opens remain outside the MMEffect ANSI import mapping");
    file = OpenEffectFile(alias.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "explicitly registered notification permits native MME to create its EMM1");
    DWORD written = 0;
    Check(WriteFile(file, "mapped-save", 11, &written, nullptr) && written == 11, "write native MME bytes to registered destination");
    CloseHandle(file);
    Check(Bytes(effect) == "mapped-save" && Bytes(baseEffect) == "base-effect-must-stay" && !fs::exists(virtualEffect),
        "registered write changes only the retained EMM and never the base EMM or virtual filename");
    Write(effect, assignment);
    const auto unregistered = folder / L"~mms-unregistered.emm";
    Write(unregistered, "unregistered"); const auto unregisteredBytes = NativeBytes(unregistered);
    file = OpenEffectFile(unregisteredBytes.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE && WriteFile(file, "native", 6, &written, nullptr) && written == 6,
        "an unregistered lookalike remains an ordinary native write"); CloseHandle(file);
    Check(Bytes(unregistered) == "native" && Bytes(effect) == assignment && Bytes(baseEffect) == "base-effect-must-stay",
        "lookalike virtual names cannot redirect writes to any project EMM");
    HANDLE locked = CreateFileW(effect.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock retained EMM against a registered read");
    file = OpenEffectFile(alias.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
        "a registered read preserves its destination sharing failure instead of falling back"); CloseHandle(locked);
    const auto unallocatedProject = folder / L"unallocated.pmm11";
    failAllocation = true; SetLastError(6618);
    const auto unavailable = EffectProjectNotificationPath(unallocatedProject.c_str());
    const DWORD unavailableError = GetLastError(); failAllocation = false;
    Check(unavailable.empty() && unavailableError == 6618 && projectAliases.size() == before + 1,
        "allocation failure returns an empty notification without publishing a partial alias or changing last error");
    for (const DWORD access : {DWORD(GENERIC_READ), DWORD(GENERIC_WRITE)}) {
        failAllocation = true;
        file = OpenEffectFile(alias.c_str(), access, 0, nullptr, access == GENERIC_READ ? OPEN_EXISTING : CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD error = GetLastError(); failAllocation = false;
        Check(file == INVALID_HANDLE_VALUE && error == ERROR_NOT_ENOUGH_MEMORY && Bytes(effect) == assignment &&
            !fs::exists(virtualEffect), "failed mapped-read or mapped-write allocation cannot access a physical virtual file");
    }
    failAllocation = true;
    const auto failedAttributes = EffectFileAttributes(alias.c_str());
    const DWORD attributesError = GetLastError(); failAllocation = false;
    Check(failedAttributes == INVALID_FILE_ATTRIBUTES && attributesError == ERROR_NOT_ENOUGH_MEMORY,
        "failed exact alias allocation retains the Win32 attribute error contract");
    Write(virtualEffect, "unrelated-virtual-file");
    for (const DWORD access : {DWORD(GENERIC_READ), DWORD(GENERIC_WRITE)}) {
        file = OpenEffectFile(alias.c_str(), access, 0, nullptr, access == GENERIC_READ ? OPEN_EXISTING : CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS,
            "a real file occupying a registered virtual EMM refuses both reads and writes");
    }
    Check(EffectFileAttributes(alias.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_EXISTS &&
        Bytes(virtualEffect) == "unrelated-virtual-file" && Bytes(effect) == assignment,
        "virtual EMM conflict preserves both the real occupant and retained effects");
    const fs::path replacement = EffectProjectNotificationPath(project.c_str());
    Check(!replacement.empty() && replacement != notification && projectAliases.size() == before + 2,
        "collision recovery publishes a new alias without retargeting a queued old alias");
    Check(DeleteFileW(virtualEffect.c_str()) != FALSE, "remove only this test's colliding EMM file");
    Write(notification, "unrelated-virtual-project");
    file = OpenEffectFile(alias.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS && Bytes(effect) == assignment &&
        Bytes(notification) == "unrelated-virtual-project", "a real virtual-PMM occupant also prevents derived EMM writes");
    Check(DeleteFileW(notification.c_str()) != FALSE && Read(alias.c_str()) == assignment,
        "an original queued alias still resolves its original EMM after the external collision is removed");
    const auto nextSequence = projectAliasSequence + 1;
    const auto collisionName = [&](std::uint64_t sequence, const wchar_t* extension) {
        return folder / (L"~mms-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence) + extension);
    };
    const auto occupiedPmm = collisionName(nextSequence, L".pmm"), occupiedEmm = collisionName(nextSequence + 1, L".emm");
    Write(occupiedPmm, "existing-pmm"); Write(occupiedEmm, "existing-emm");
    const fs::path afterCollisions = EffectProjectNotificationPath((folder / L"other.pmm111").c_str());
    auto afterCollisionEffect = afterCollisions; afterCollisionEffect.replace_extension(L".emm");
    Check(!afterCollisions.empty() && afterCollisions != occupiedPmm && afterCollisionEffect != occupiedEmm &&
        Bytes(occupiedPmm) == "existing-pmm" && Bytes(occupiedEmm) == "existing-emm",
        "alias registration skips existing PMM and EMM spellings without changing either occupant");
    Write(folder / L"other.emm111", "independent-other-version");
    Check(Read(NativeBytes(afterCollisionEffect).c_str()) == "independent-other-version" && Read(alias.c_str()) == assignment,
        "separately queued suffix notifications retain distinct original EMM destinations");
    Check(DeleteFileW(occupiedPmm.c_str()) && DeleteFileW(occupiedEmm.c_str()), "remove only owned initial-collision fixtures");
    const auto retained = projectAliases.size();
    // White-box capacity injection avoids thousands of disk probes while the
    // public boundary still executes its real refusal and stable-reuse paths.
    while (projectAliases.size() < kMaxProjectAliases) {
        const auto number = std::to_wstring(projectAliases.size());
        projectAliases.push_back({folder / (L"~mms-capacity-" + number + L".emm"), folder / (L"capacity-" + number + L".emm1")});
    }
    SetLastError(6619);
    const auto full = EffectProjectNotificationPath((folder / L"over-capacity.pmm1").c_str());
    const DWORD fullError = GetLastError();
    Check(full.empty() && fullError == 6619 && projectAliases.size() == kMaxProjectAliases &&
        EffectProjectNotificationPath(project.c_str()) == notification.wstring(),
        "full alias capacity refuses new registrations but preserves a previously queued stable mapping");
    projectAliases.resize(retained);
    const auto sequence = projectAliasSequence;
    projectAliasSequence = std::numeric_limits<std::uint64_t>::max();
    const auto exhaustedProject = folder / L"sequence-exhausted.pmm1";
    SetLastError(6620);
    const auto exhausted = EffectProjectNotificationPath(exhaustedProject.c_str());
    const DWORD exhaustedError = GetLastError();
    Check(exhausted.empty() && exhaustedError == 6620 && projectAliases.size() == retained &&
        projectAliasSequence == std::numeric_limits<std::uint64_t>::max(),
        "exhausted alias sequence fails without wrapping into an earlier queued identity");
    projectAliasSequence = sequence;
    PrepareEffectProjectPaths(nullptr);
}
void AssignmentPaths(const fs::path& root) {
    const auto project = root / L"projects" / L"scene.pmm";
    auto emm = project; emm.replace_extension(L".emm");
    const auto local = project.parent_path() / L"effects" / L"assigned.fx";
    const auto legacy = root / L"effects" / L"assigned.fx";
    Write(local, "project-red"); Write(legacy, "exe-green");
    const std::string assignment = "[Info]\r\nVersion = 3\r\n[Effect]\r\nPmd1 = \"effects/assigned.fx\"\r\n";
    Write(emm, assignment);
    fs::create_directory(root / L"browse"); fs::current_path(root / L"browse");
    SetLastError(4411); PrepareEffectProjectPaths(project.c_str());
    Check(GetLastError() == 4411, "project preparation preserves caller last error");
    const auto path = NativeBytes(legacy);
    Check(Read(path.c_str()) == "project-red", "project-relative file precedes an existing exe-relative file");
    PrepareEffectProjectPaths((root / L"another-model.pmx").c_str());
    Check(Read(path.c_str()) == "project-red", "model notification does not replace project-relative aliases");
    Check(EffectFileAttributes(path.c_str()) != INVALID_FILE_ATTRIBUTES, "attribute probe sees project-relative effect");
    std::array<wchar_t, 32768> converted{};
    const int size = DecodeEffectPath(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    Check(size == int(legacy.native().size() + 1), "generic conversion retains the original path and terminator");
    SetLastError(4422);
    Check(DecodeEffectPath(CP_ACP, 0, path.c_str(), -1, converted.data(), int(converted.size())) == size &&
        fs::path(converted.data()) == legacy && GetLastError() == 4422, "generic conversion repairs encoding without applying project aliases");
    Check(DecodeEffectPath(CP_ACP, 0, path.c_str(), -1, converted.data(), size - 1) == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
        "conversion retains insufficient-buffer contract");
    Check(DecodeEffectPath(CP_ACP, 0, path.c_str(), -1, converted.data(), -1) == 0 && GetLastError() == ERROR_INVALID_PARAMETER,
        "conversion rejects negative capacity");
    alignas(wchar_t) std::array<char, 2048> overlapping{};
    for (int capacity : {1, 1024}) {
        std::strcpy(overlapping.data(), path.c_str());
        const auto original = MultiByteToWideChar(CP_ACP, 0, overlapping.data(), -1,
            reinterpret_cast<wchar_t*>(overlapping.data()), capacity);
        const auto error = GetLastError();
        std::strcpy(overlapping.data(), path.c_str());
        Check(original == 0 && DecodeEffectPath(CP_ACP, 0, overlapping.data(), -1,
            reinterpret_cast<wchar_t*>(overlapping.data()), capacity) == original && GetLastError() == error,
            "mapped conversion retains native same-address invalid-parameter result");
    }
    Check(DecodeEffectPath(CP_ACP, 0, overlapping.data(), -1, reinterpret_cast<wchar_t*>(overlapping.data()), 0) == size,
        "same-address size query remains permitted without writing output");
    for (DWORD access : {DWORD(GENERIC_WRITE), DWORD(GENERIC_READ | GENERIC_WRITE), DWORD(FILE_APPEND_DATA)}) {
        HANDLE file = OpenEffectFile(path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "write-access open retains native target");
        LARGE_INTEGER end{}; SetFilePointerEx(file, end, nullptr, FILE_END); DWORD count = 0;
        Check(WriteFile(file, "+", 1, &count, nullptr) && count == 1, "write native target"); CloseHandle(file);
        Check(Bytes(local) == "project-red", "write requests never use read alias");
    }
    HANDLE disposable = OpenEffectFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    Check(disposable != INVALID_HANDLE_VALUE, "open delete-on-close native fixture"); CloseHandle(disposable);
    Check(!fs::exists(legacy) && Bytes(local) == "project-red", "delete-on-close is never classified as a read-only alias request");
    Write(legacy, "exe-green");
    Check(DecodeEffectPath(CP_ACP, 0, path.c_str(), -1, converted.data(), int(converted.size())) == size,
        "restore a complete generic conversion after argument failure checks");
    HANDLE wideWrite = CreateFileW(converted.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(wideWrite != INVALID_HANDLE_VALUE, "wide write after generic conversion opens the original file");
    DWORD written = 0;
    Check(WriteFile(wideWrite, "native", 6, &written, nullptr) && written == 6, "write actual converted wide filename");
    CloseHandle(wideWrite);
    Check(Bytes(local) == "project-red" && Bytes(legacy).find("native") == 0,
        "generic conversion followed by wide write cannot write through the project alias");
    Check(DeleteFileW(converted.data()) && !fs::exists(legacy) && Bytes(local) == "project-red",
        "generic conversion followed by wide deletion only deletes the original file");
    Write(legacy, "exe-green");
    HANDLE locked = CreateFileW(local.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "exclusively lock project effect while different EXE effect exists");
    HANDLE refused = OpenEffectFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(refused == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
        "locked project alias retains its failure instead of reading the EXE effect");
    CloseHandle(locked);
    const auto missing = NativeBytes(root / L"absent.fx");
    GetFileAttributesA(missing.c_str()); const DWORD missingError = GetLastError();
    Check(EffectFileAttributes(missing.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == missingError,
        "missing attribute retains native failure code");
    failAllocation = true;
    HANDLE file = OpenEffectFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    failAllocation = false;
    Check(file != INVALID_HANDLE_VALUE, "allocation failure cannot escape the native open wrapper"); CloseHandle(file);
    failAllocation = true; PrepareEffectProjectPaths(project.c_str()); failAllocation = false;
    Check(Read(path.c_str()) == "exe-green", "failed new project preparation retires old aliases");
    PrepareEffectProjectPaths(project.c_str());
    const auto emmPath = NativeBytes(emm);
    PrepareEffectProjectPaths(nullptr);
    Check(Read(path.c_str()) == "exe-green", "workspace reset retires relative aliases");
    file = OpenEffectFile(emmPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "native MME menu can open its EMM exclusively");
    LARGE_INTEGER zero{}, position{}, saved{}; saved.QuadPart = 7;
    Check(SetFilePointerEx(file, zero, &position, FILE_CURRENT) && position.QuadPart == 0,
        "exclusive EMM probe returns the newly opened handle at its initial cursor");
    Check(SetFilePointerEx(file, saved, nullptr, FILE_BEGIN), "set a nonzero native EMM cursor");
    SetLastError(4433);
    Check(TrackAssignmentRead(file, true, FILE_ATTRIBUTE_NORMAL) == file && GetLastError() == 4433 &&
        SetFilePointerEx(file, zero, &position, FILE_CURRENT) && position.QuadPart == 7,
        "same-handle EMM parser preserves a nonzero cursor and incoming last error");
    char nextByte = 0; DWORD read = 0;
    Check(ReadFile(file, &nextByte, 1, &read, nullptr) && read == 1 && nextByte == assignment[7],
        "caller consumes the next original byte after compatibility parsing");
    CloseHandle(file);
    Check(Read(path.c_str()) == "project-red", "direct EMM reads install aliases from its actual directory");
    Check(Bytes(emm) == assignment, "compatibility never rewrites assignment contents");
    file = CreateFileW(emm.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE && SetFilePointerEx(file, saved, nullptr, FILE_BEGIN),
        "prepare an exclusively owned EMM for partial-read failure");
    faultFile = file; failRead = true; SetLastError(4444);
    Check(TrackAssignmentRead(file, true, FILE_ATTRIBUTE_NORMAL) == file && GetLastError() == 4444 && partialReads == 1 &&
        SetFilePointerEx(file, zero, &position, FILE_CURRENT) && position.QuadPart == 7,
        "a partial failed read keeps the native handle cursor and original last error");
    faultFile = INVALID_HANDLE_VALUE; CloseHandle(file);
    Check(Read(path.c_str()) == "exe-green", "partial EMM reads never publish partial or stale aliases");
    PrepareEffectProjectPaths(project.c_str());
    file = CreateFileW(emm.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE && SetFilePointerEx(file, saved, nullptr, FILE_BEGIN),
        "prepare EMM cursor restoration failure");
    faultFile = file; failRestore = true;
    Check(TrackAssignmentRead(file, true, FILE_ATTRIBUTE_NORMAL) == INVALID_HANDLE_VALUE &&
        GetLastError() == ERROR_SEEK && restoreFailures == 1,
        "failed cursor restoration refuses the modified successful handle with its actual error");
    BY_HANDLE_FILE_INFORMATION information{};
    Check(!GetFileInformationByHandle(file, &information) && GetLastError() == ERROR_INVALID_HANDLE,
        "cursor restoration failure closes its private handle exactly once");
    faultFile = INVALID_HANDLE_VALUE;
    Check(Read(path.c_str()) == "exe-green", "failed cursor restoration does not publish an assignment");
    PrepareEffectProjectPaths(project.c_str());
    file = OpenEffectFile(emmPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    Check(file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, &information),
        "asynchronous EMM opens retain a usable native handle without synchronous probing");
    CloseHandle(file);
    Check(Read(path.c_str()) == "exe-green", "a newly selected asynchronous EMM cannot retain another assignment");
    PrepareEffectProjectPaths((root / L"no-sidecar.pmm").c_str());
    Check(Read(path.c_str()) == "exe-green", "opening a project without EMM cannot retain previous aliases");
    for (const auto& sample : {std::pair<UINT, const wchar_t*>{932, L"ソフト"}, {936, L"中文效果"}, {65001, L"中文效果"}}) {
        const auto relative = fs::path(sample.second) / L"assigned.fx";
        Write(project.parent_path() / relative, "encoded-blue");
        std::string encoded; Check(text_encoding::EncodeExact(relative.c_str(), sample.first, encoded), "encode exact foreign EMM path");
        Write(emm, "[Effect]\nPmd1 = " + encoded + "\n"); PrepareEffectProjectPaths(project.c_str());
        const auto absolute = root / relative;
        Check(text_encoding::EncodeExact(absolute.c_str(), sample.first, encoded), "encode MME expanded path");
        Check(Read(encoded.c_str()) == "encoded-blue", "foreign path and CP932 trail separator resolve against project directory");
    }
    Write(emm, "[Effect]\nPmd1 = effects/assigned.fx\n"); PrepareEffectProjectPaths(project.c_str());
    Write(emm, std::string(16 * 1024 * 1024 + 1, 'x')); PrepareEffectProjectPaths(project.c_str());
    Check(Read(path.c_str()) == "exe-green", "oversized assignment is bounded and does not preserve aliases");
    file = CreateFileW(emm.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE && SetFilePointerEx(file, saved, nullptr, FILE_BEGIN), "open an oversized EMM handle");
    SetLastError(4455);
    Check(TrackAssignmentRead(file, true, FILE_ATTRIBUTE_NORMAL) == file && GetLastError() == 4455 &&
        SetFilePointerEx(file, zero, &position, FILE_CURRENT) && position.QuadPart == 7,
        "oversized same-handle assignments leave the native cursor and last error intact");
    CloseHandle(file);
    SetLastError(4466);
    Check(TrackAssignmentRead(INVALID_HANDLE_VALUE, true, FILE_ATTRIBUTE_NORMAL) == INVALID_HANDLE_VALUE && GetLastError() == 4466,
        "failed native opens are not parsed or given a replacement error");
    PrepareEffectProjectPaths(nullptr);
}
HRESULT WINAPI CreateMock(IDirect3DDevice9*, LPCWSTR, const void*, const void*, DWORD, void*, void** effect, void**) {
    Check(GetLastError() == 5533, "effect diagnostics restore incoming error before calling original compiler");
    if (effect) *effect = reinterpret_cast<void*>(0x1234);
    SetLastError(5544); return E_FAIL;
}
HRESULT WINAPI ReadCompilerMock(IDirect3DDevice9*, LPCWSTR filename, const void*, const void*, DWORD, void*, void**, void**) {
    Check(GetLastError() == 5533, "compiler redirection retains incoming last error");
    wcscpy_s(compilerPath, filename ? filename : L"");
    compiledLength = 0;
    HANDLE file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const bool read = ReadFile(file, compiledBytes, sizeof(compiledBytes), &compiledLength, nullptr) != FALSE;
    const DWORD error = read ? 5544 : GetLastError();
    CloseHandle(file); SetLastError(error);
    return read ? S_OK : HRESULT_FROM_WIN32(error);
}
void CompilerDiagnostics(const fs::path& root) {
    std::array<void*, 107> table{};
    for (auto& slot : table) slot = reinterpret_cast<void*>(&CreateMock);
    void* object = table.data(); void* effect = nullptr;
    originalEffectFile.store(&CreateMock);
    const unsigned before = traces; SetLastError(5533);
    Check(TraceEffectFile(reinterpret_cast<IDirect3DDevice9*>(&object), L"fixture.fx", nullptr, nullptr, 0,
        &object, &effect, nullptr) == E_FAIL && effect == reinterpret_cast<void*>(0x1234) && GetLastError() == 5544,
        "effect diagnostics preserve original HRESULT output and last error");
    Check(traces >= before + 14, "effect file and device/pool dispatch targets are traced");
    auto* guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Check(guard != nullptr, "reserve unreadable dispatch object");
    SetLastError(5533);
    Check(TraceEffectFile(reinterpret_cast<IDirect3DDevice9*>(guard), nullptr, nullptr, nullptr, 0,
        guard, nullptr, nullptr) == E_FAIL && GetLastError() == 5544, "unreadable device and pool diagnostics do not dereference them");
    originalEffectFile.store(nullptr);
    Check(TraceEffectFile(nullptr, nullptr, nullptr, nullptr, 0, nullptr, static_cast<void**>(guard), nullptr) == E_NOINTERFACE,
        "missing compiler and unreadable output slot do not crash diagnostics");
    VirtualFree(guard, 0, MEM_RELEASE);
    const auto project = root / L"compiler" / L"scene.pmm";
    auto emm = project; emm.replace_extension(L".emm");
    const auto local = project.parent_path() / L"effects" / L"compile.fx";
    const auto legacy = root / L"effects" / L"compile.fx";
    Write(local, "compile-project"); Write(legacy, "compile-exe");
    Write(emm, "[Effect]\nPmd1=effects/compile.fx\n");
    PrepareEffectProjectPaths(project.c_str());
    originalEffectFile.store(&ReadCompilerMock);
    SetLastError(5533);
    Check(TraceEffectFile(nullptr, legacy.c_str(), nullptr, nullptr, 0, nullptr, nullptr, nullptr) == S_OK &&
        GetLastError() == 5544 && fs::path(compilerPath) == local && fs::path(tracedPath) == local &&
        std::string(compiledBytes, compiledLength) == "compile-project",
        "known read-only effect compilation receives and traces the exact project-relative file");
    HANDLE locked = CreateFileW(local.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock project compiler input while the EXE input remains readable");
    SetLastError(5533);
    Check(TraceEffectFile(nullptr, legacy.c_str(), nullptr, nullptr, 0, nullptr, nullptr, nullptr) ==
        HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) && GetLastError() == ERROR_SHARING_VIOLATION &&
        fs::path(compilerPath) == local && fs::path(tracedPath) == local,
        "compiler retains its original sharing HRESULT and target without falling back to a different effect");
    CloseHandle(locked);
    for (const auto& sample : {std::pair<UINT, const wchar_t*>{932, L"ソフト"}, {936, L"中文编译"}, {65001, L"中文编译"}}) {
        const auto relative = fs::path(sample.second) / L"foreign.fx";
        const auto localForeign = project.parent_path() / relative;
        const auto legacyForeign = root / relative;
        Write(localForeign, "foreign-project");
        Check(!fs::exists(legacyForeign), "foreign compiler fixture exists only inside the project directory");
        std::string assignment, encoded;
        Check(text_encoding::EncodeExact(relative.c_str(), sample.first, assignment) &&
            text_encoding::EncodeExact(legacyForeign.c_str(), sample.first, encoded),
            "encode foreign EMM and compiler paths in the same source code page");
        Write(emm, "[Effect]\nPmd1=" + assignment + "\n");
        PrepareEffectProjectPaths(project.c_str());
        std::array<wchar_t, 32768> converted{};
        SetLastError(5533);
        Check(DecodeEffectPath(CP_ACP, 0, encoded.c_str(), -1, converted.data(), int(converted.size())) > 0 &&
            GetLastError() == 5533 && fs::path(converted.data()) == legacyForeign,
            "project-only foreign assignment repairs code page without changing the generic filename");
        SetLastError(5533);
        Check(TraceEffectFile(nullptr, converted.data(), nullptr, nullptr, 0, nullptr, nullptr, nullptr) == S_OK &&
            GetLastError() == 5544 && fs::path(compilerPath) == localForeign && fs::path(tracedPath) == localForeign &&
            std::string(compiledBytes, compiledLength) == "foreign-project",
            "generic decode and read-only compilation compose for a project-only foreign path");
    }
    Write(emm, "[Effect]\nPmd1=effects/compile.fx\n"); PrepareEffectProjectPaths(project.c_str());
    failAllocation = true; SetLastError(5533);
    const auto allocationFallback = TraceEffectFile(nullptr, legacy.c_str(), nullptr, nullptr, 0, nullptr, nullptr, nullptr);
    failAllocation = false;
    Check(allocationFallback == S_OK && GetLastError() == 5544 && fs::path(compilerPath) == legacy &&
        fs::path(tracedPath) == legacy && std::string(compiledBytes, compiledLength) == "compile-exe",
        "failed path preparation keeps the original compiler input HRESULT and last error");
    originalEffectFile.store(nullptr); PrepareEffectProjectPaths(nullptr);
}
// GetFullPathNameA is a native ANSI API whose Win32 edge contract must survive
// the compatibility entry. These helpers observe the whole output buffer with
// guards, the return value, LastError and the lpFilePart position. The expected
// identity comes from the real files and the actual EMM fixture below, never
// from the wrapper's own result.
constexpr DWORD kFullPathIncomingError = 0x2710;
constexpr std::size_t kFullPathGuardBytes = 16;
constexpr char kFullPathSentinel = '\x5A';
struct FullPathCase { DWORD capacity; bool nullBuffer; };
struct FullPathObservation {
    std::vector<char> storage;
    DWORD returned = 0;
    DWORD error = 0;
    bool partRequested = false;
    bool partNull = false;
    bool partInside = false;
    std::size_t partOffset = 0;
};
using FullPathApi = DWORD(WINAPI*)(LPCSTR, DWORD, LPSTR, LPSTR*);
DWORD WINAPI ThirdPartyFullPathNameA(LPCSTR, DWORD, LPSTR, LPSTR*) {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return 0;
}
FullPathObservation ObserveFullPath(FullPathApi api, const char* input, DWORD capacity,
                                    bool nullBuffer, bool requestPart,
                                    bool allocationFault = false) {
    FullPathObservation observed;
    const std::size_t bufferBytes = nullBuffer ? 0 : static_cast<std::size_t>(capacity);
    observed.storage.assign(kFullPathGuardBytes + bufferBytes + kFullPathGuardBytes,
                            kFullPathSentinel);
    LPSTR buffer = nullBuffer ? nullptr : observed.storage.data() + kFullPathGuardBytes;
    LPSTR part = nullptr;
    observed.partRequested = requestPart;
    SetLastError(kFullPathIncomingError);
    if (allocationFault) failAllocation = true;
    observed.returned = api(input, capacity, buffer, requestPart ? &part : nullptr);
    if (allocationFault) failAllocation = false;
    observed.error = GetLastError();
    observed.partNull = part == nullptr;
    if (part) {
        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(buffer);
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(part);
        if (buffer && value >= base && value - base <= bufferBytes) {
            observed.partInside = true;
            observed.partOffset = static_cast<std::size_t>(value - base);
        }
    }
    return observed;
}
std::string FullPathText(FullPathApi api, const char* input) {
    std::vector<char> buffer(32768, kFullPathSentinel);
    SetLastError(kFullPathIncomingError);
    const DWORD returned = api(input, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    std::size_t length = 0;
    while (length < buffer.size() && buffer[length] != '\0') ++length;
    if (!returned || length == buffer.size()) return {};
    return std::string(buffer.data(), length);
}
bool SameFullPathText(const std::string& ansi, const fs::path& expected) {
    std::wstring decoded;
    return text_encoding::Decode(ansi.c_str(), GetACP(), decoded) &&
        _wcsicmp(fs::path(decoded).lexically_normal().c_str(),
                 expected.lexically_normal().c_str()) == 0;
}
void CheckFullPathContract(const char* label, const char* input, const char* expectedSource,
                           bool allocationFault = false) {
    SetLastError(kFullPathIncomingError);
    const DWORD required = ::GetFullPathNameA(expectedSource, 0, nullptr, nullptr);
    std::array<FullPathCase, 5> probes{};
    std::size_t count = 0;
    probes[count++] = {0, true};
    probes[count++] = {1, false};
    if (required > 1) probes[count++] = {required - 1, false};
    if (required) probes[count++] = {required, false};
    probes[count++] = {required + 32, false};
    for (std::size_t index = 0; index < count; ++index) {
        const FullPathCase& probe = probes[index];
        const FullPathObservation expected = ObserveFullPath(&::GetFullPathNameA, expectedSource,
            probe.capacity, probe.nullBuffer, !probe.nullBuffer);
        const FullPathObservation actual = ObserveFullPath(&EffectFullPathNameA, input,
            probe.capacity, probe.nullBuffer, !probe.nullBuffer, allocationFault);
        char message[256];
        std::snprintf(message, sizeof(message),
            "GetFullPathNameA contract [%s] capacity=%lu%s preserves the native return length",
            label, static_cast<unsigned long>(probe.capacity),
            probe.nullBuffer ? " null buffer" : "");
        Check(actual.returned == expected.returned, message);
        std::snprintf(message, sizeof(message),
            "GetFullPathNameA contract [%s] capacity=%lu%s preserves the native LastError",
            label, static_cast<unsigned long>(probe.capacity),
            probe.nullBuffer ? " null buffer" : "");
        Check(actual.error == expected.error, message);
        std::snprintf(message, sizeof(message),
            "GetFullPathNameA contract [%s] capacity=%lu%s preserves the native lpFilePart position",
            label, static_cast<unsigned long>(probe.capacity),
            probe.nullBuffer ? " null buffer" : "");
        Check(actual.partRequested == expected.partRequested && actual.partNull == expected.partNull &&
            actual.partInside == expected.partInside && actual.partOffset == expected.partOffset, message);
        std::snprintf(message, sizeof(message),
            "GetFullPathNameA contract [%s] capacity=%lu%s preserves the whole output buffer",
            label, static_cast<unsigned long>(probe.capacity),
            probe.nullBuffer ? " null buffer" : "");
        Check(actual.storage == expected.storage, message);
    }
}
// The mapped identity is defined by the real project/EXE files and the real EMM
// assignment. Native GetFullPathNameA is the only reference for buffer sizing,
// output text, LastError and lpFilePart.
void FullPathNameCompatibility(const fs::path& root) {
    const auto savedCwd = fs::current_path();
    PrepareEffectProjectPaths(nullptr);
    const auto fixture = root / (L"fullpath-fixture-" + std::to_wstring(GetCurrentProcessId()));
    const auto projectDirectory = fixture / L"project";
    const auto project = projectDirectory / L"scene.pmm";
    auto emm = project; emm.replace_extension(L".emm");
    const auto otherDirectory = fixture / L"other";
    const fs::path relative = fs::path(L"effects") / L"name.fx";
    const auto legacy = root / relative;
    const auto local = projectDirectory / relative;
    const auto otherLegacy = otherDirectory / relative;
    const auto goneRelative = fs::path(L"effects") / L"gone.fx";
    const auto legacyMissing = root / goneRelative;
    const auto directoryRelative = fs::path(L"effects") / L"namdir.fx";
    const auto localDirectory = projectDirectory / directoryRelative;
    const auto legacyDirectory = root / directoryRelative;
    Write(legacy, "exe-name"); Write(local, "project-name"); Write(otherLegacy, "other-name");
    Write(legacyMissing, "exe-gone"); Write(legacyDirectory, "exe-namdir");
    fs::create_directories(localDirectory);
    Write(emm, "[Effect]\nPmd1=effects\\name.fx\nPmd2=effects\\gone.fx\nPmd3=effects\\namdir.fx\n");
    Check(fs::exists(legacy) && fs::exists(local) && Bytes(legacy) == "exe-name" &&
        Bytes(local) == "project-name",
        "same-named project and EXE effects both exist with distinct bytes");
    const std::string legacyBytes = NativeBytes(legacy);
    const std::string localBytes = NativeBytes(local);
    const std::string otherBytes = NativeBytes(otherLegacy);
    const std::string legacyMissingBytes = NativeBytes(legacyMissing);
    const std::string legacyDirectoryBytes = NativeBytes(legacyDirectory);
    const std::string relativeInput = "effects\\name.fx";
    CheckFullPathContract("no mapping", legacyBytes.c_str(), legacyBytes.c_str());
    fs::current_path(root);
    const std::string legacyExpanded = FullPathText(&::GetFullPathNameA, relativeInput.c_str());
    Check(SameFullPathText(legacyExpanded, legacy),
        "native expansion of the legacy relative spelling names the EXE fixture");
    CheckFullPathContract("no mapping relative", relativeInput.c_str(), relativeInput.c_str());
    PrepareEffectProjectPaths(project.c_str());
    Check(Read(legacyExpanded.c_str()) == "project-name",
        "the real EMM mapping reads the project effect instead of the EXE effect");
    const std::string mappedByNative = FullPathText(&::GetFullPathNameA, localBytes.c_str());
    const std::string mappedByWrapper = FullPathText(&EffectFullPathNameA, legacyExpanded.c_str());
    Check(!mappedByNative.empty() && mappedByWrapper == mappedByNative,
        "mapped legacy absolute path returns the native project full path");
    Check(SameFullPathText(mappedByWrapper, local) && !SameFullPathText(mappedByWrapper, legacy),
        "mapped legacy absolute path names the project effect, not the same-named EXE effect");
    CheckFullPathContract("mapped absolute", legacyExpanded.c_str(), localBytes.c_str());
    CheckFullPathContract("mapped relative", relativeInput.c_str(), localBytes.c_str());
    fs::current_path(otherDirectory);
    const std::string otherExpanded = FullPathText(&::GetFullPathNameA, relativeInput.c_str());
    Check(SameFullPathText(otherExpanded, otherLegacy),
        "the other cwd natively expands the same relative spelling to its own effect");
    const std::string otherNative = FullPathText(&::GetFullPathNameA, relativeInput.c_str());
    const std::string otherWrapper = FullPathText(&EffectFullPathNameA, relativeInput.c_str());
    Check(otherWrapper == otherNative && SameFullPathText(otherWrapper, otherLegacy) &&
        !SameFullPathText(otherWrapper, local),
        "another cwd keeps its own native expansion instead of guessing the EXE effect");
    CheckFullPathContract("other cwd relative", relativeInput.c_str(), relativeInput.c_str());
    CheckFullPathContract("absolute non-alias", otherBytes.c_str(), otherBytes.c_str());
    CheckFullPathContract("missing mapped local", legacyMissingBytes.c_str(), legacyMissingBytes.c_str());
    CheckFullPathContract("directory mapped local", legacyDirectoryBytes.c_str(), legacyDirectoryBytes.c_str());
    const std::string unrelatedBytes = NativeBytes(fixture / L"unrelated.fx");
    CheckFullPathContract("unrelated absolute", unrelatedBytes.c_str(), unrelatedBytes.c_str());
    CheckFullPathContract("empty input", "", "");
    CheckFullPathContract("allocation fallback", legacyExpanded.c_str(), legacyExpanded.c_str(), true);
    Check(!failAllocation, "allocation fault injection is cleared before assertions");
    PrepareEffectProjectPaths(nullptr);
    const std::string clearedNative = FullPathText(&::GetFullPathNameA, legacyExpanded.c_str());
    const std::string clearedWrapper = FullPathText(&EffectFullPathNameA, legacyExpanded.c_str());
    Check(clearedWrapper == clearedNative && SameFullPathText(clearedWrapper, legacy),
        "cleared EMM mapping stops redirecting the former legacy path");
    CheckFullPathContract("cleared mapping", legacyExpanded.c_str(), legacyExpanded.c_str());
    fs::current_path(savedCwd);
    PrepareEffectProjectPaths(nullptr);
}
// Targets the active ACP cannot spell are persisted as strict UTF-8 bytes. That
// branch has its own byte contract, so its expected text comes from the real
// fixture path through the strict encoder, never from an ANSI expansion of the
// wrapper's own result. The guarded probes observe the returned byte count, the
// terminator, both canary regions, lpFilePart and LastError independently.
std::wstring NormalizedWidePath(const fs::path& path) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                            buffer.data(), nullptr);
    Check(length > 0 && length < static_cast<DWORD>(buffer.size()),
        "native wide normalization resolves an owned fixture path");
    return std::wstring(buffer.data(), length);
}
bool StrictUtf8Path(const fs::path& path, std::wstring& normalized, std::string& bytes) {
    normalized = NormalizedWidePath(path);
    return text_encoding::EncodeExact(normalized.c_str(), CP_UTF8, bytes);
}
struct Utf8FullPathCase { DWORD capacity; bool nullBuffer; };
struct Utf8FullPathObservation {
    std::vector<char> storage;
    DWORD returned = 0;
    DWORD error = 0;
    bool partUnchanged = false;
    bool partInside = false;
    std::size_t partOffset = 0;
};
Utf8FullPathObservation ObserveUtf8FullPath(const char* input, DWORD capacity,
                                            bool nullBuffer, bool requestPart) {
    static char partSentinel = '\x7A';
    Utf8FullPathObservation observed;
    const std::size_t bufferBytes = nullBuffer ? 0 : static_cast<std::size_t>(capacity);
    observed.storage.assign(kFullPathGuardBytes + bufferBytes + kFullPathGuardBytes,
                            kFullPathSentinel);
    LPSTR buffer = nullBuffer ? nullptr : observed.storage.data() + kFullPathGuardBytes;
    LPSTR part = &partSentinel;
    SetLastError(kFullPathIncomingError);
    observed.returned = EffectFullPathNameA(input, capacity, buffer,
                                            requestPart ? &part : nullptr);
    observed.error = GetLastError();
    observed.partUnchanged = part == &partSentinel;
    if (buffer && !observed.partUnchanged) {
        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(buffer);
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(part);
        if (value >= base && value - base <= bufferBytes) {
            observed.partInside = true;
            observed.partOffset = static_cast<std::size_t>(value - base);
        }
    }
    return observed;
}
void CheckUtf8FullPathContract(const char* label, const char* input, const fs::path& target) {
    std::wstring targetWide;
    std::string expected;
    Check(StrictUtf8Path(target, targetWide, expected),
        "the real fixture target has an exact strict UTF-8 reference");
    std::string acp;
    if (text_encoding::EncodeExact(targetWide.c_str(), GetACP(), acp)) {
        std::printf("NOTE GetFullPathNameA UTF-8 contract [%s]: ACP %u can spell the target; "
                    "the persisted UTF-8 branch is not constructible on this host.\n",
                    label, static_cast<unsigned>(GetACP()));
        return;
    }
    const DWORD required = static_cast<DWORD>(expected.size() + 1);
    std::size_t expectedPart = 0;
    for (std::size_t index = expected.size(); index > 0; --index) {
        if (expected[index - 1] == '\\' || expected[index - 1] == '/') {
            expectedPart = index;
            break;
        }
    }
    std::array<Utf8FullPathCase, 6> probes{};
    std::size_t count = 0;
    probes[count++] = {0, true};
    probes[count++] = {0, false};
    probes[count++] = {1, false};
    if (required > 1) probes[count++] = {required - 1, false};
    probes[count++] = {required, false};
    probes[count++] = {required + 32, false};
    for (const bool requestPart : {false, true}) {
        for (std::size_t index = 0; index < count; ++index) {
            const Utf8FullPathCase& probe = probes[index];
            const std::size_t bufferBytes = probe.nullBuffer
                ? 0 : static_cast<std::size_t>(probe.capacity);
            std::vector<char> expectedStorage(
                kFullPathGuardBytes + bufferBytes + kFullPathGuardBytes, kFullPathSentinel);
            const bool writes = !probe.nullBuffer && probe.capacity >= required;
            if (writes) {
                std::memcpy(expectedStorage.data() + kFullPathGuardBytes, expected.data(),
                            expected.size());
                expectedStorage[kFullPathGuardBytes + expected.size()] = '\0';
            }
            const Utf8FullPathObservation actual = ObserveUtf8FullPath(
                input, probe.capacity, probe.nullBuffer, requestPart);
            char message[320];
            std::snprintf(message, sizeof(message),
                "UTF-8 full path contract [%s] capacity=%lu%s part=%s preserves the byte return length",
                label, static_cast<unsigned long>(probe.capacity),
                probe.nullBuffer ? " null buffer" : "", requestPart ? "yes" : "no");
            Check(actual.returned == (writes ? static_cast<DWORD>(expected.size()) : required),
                message);
            std::snprintf(message, sizeof(message),
                "UTF-8 full path contract [%s] capacity=%lu%s part=%s keeps the LastError sentinel",
                label, static_cast<unsigned long>(probe.capacity),
                probe.nullBuffer ? " null buffer" : "", requestPart ? "yes" : "no");
            Check(actual.error == kFullPathIncomingError, message);
            std::snprintf(message, sizeof(message),
                "UTF-8 full path contract [%s] capacity=%lu%s part=%s keeps the whole guarded buffer",
                label, static_cast<unsigned long>(probe.capacity),
                probe.nullBuffer ? " null buffer" : "", requestPart ? "yes" : "no");
            Check(actual.storage == expectedStorage, message);
            if (!writes) {
                if (requestPart) {
                    std::snprintf(message, sizeof(message),
                        "UTF-8 full path contract [%s] capacity=%lu%s part=%s leaves lpFilePart unchanged",
                        label, static_cast<unsigned long>(probe.capacity),
                        probe.nullBuffer ? " null buffer" : "", requestPart ? "yes" : "no");
                    Check(actual.partUnchanged, message);
                }
                continue;
            }
            if (requestPart) {
                std::snprintf(message, sizeof(message),
                    "UTF-8 full path contract [%s] capacity=%lu%s part=%s writes the exact byte offset",
                    label, static_cast<unsigned long>(probe.capacity),
                    probe.nullBuffer ? " null buffer" : "", requestPart ? "yes" : "no");
                Check(!actual.partUnchanged && actual.partInside &&
                    actual.partOffset == expectedPart, message);
            }
        }
    }
}
// A mapped project target the active ACP cannot name is persisted as its real
// UTF-8 absolute path. Native expansion of the legacy input names the
// same-named EXE file instead, so that ANSI spelling is not a valid reference
// for the result. If every probe character is representable the branch is not
// constructible and the fact is recorded instead of fabricating bad bytes.
void FullPathNameLosslessEncoding(const fs::path& root) {
    const wchar_t* const candidates[] = {L"\U0001F600", L"\u4E2D", L"\u0416"};
    const wchar_t* unencodable = nullptr;
    for (const wchar_t* candidate : candidates) {
        std::string encoded;
        if (!text_encoding::EncodeExact(candidate, GetACP(), encoded)) {
            unencodable = candidate; break;
        }
    }
    if (!unencodable) {
        std::printf("NOTE GetFullPathNameA: ACP %u losslessly encodes every probe character; "
                    "the persisted UTF-8 local fallback cannot be constructed on this host.\n",
                    static_cast<unsigned>(GetACP()));
        return;
    }
    const auto projectDirectory = root /
        (std::wstring(L"fullpath-acp-") + std::to_wstring(GetCurrentProcessId())) /
        std::wstring(unencodable);
    const auto project = projectDirectory / L"scene.pmm";
    auto emm = project; emm.replace_extension(L".emm");
    const fs::path relative = fs::path(L"effects") / L"name.fx";
    const auto legacy = root / relative;
    const auto local = projectDirectory / relative;
    Write(legacy, "acp-exe");
    Write(local, "acp-project");
    Write(emm, "[Effect]\nPmd1=effects\\name.fx\n");
    PrepareEffectProjectPaths(project.c_str());
    const std::string legacyBytes = NativeBytes(legacy);
    std::string encoded;
    Check(!text_encoding::EncodeExact(local.c_str(), GetACP(), encoded),
        "the mapped project path has no lossless ACP spelling");
    const std::string legacyContent = Read(legacyBytes.c_str());
    Check(legacyContent == "acp-project" && legacyContent != Bytes(legacy) &&
        Bytes(legacy) == "acp-exe" && Bytes(local) == "acp-project",
        "the real EMM mapping reads the project effect, not the same-named EXE effect");
    const std::wstring localWide = NormalizedWidePath(local);
    std::string expectedUtf8;
    Check(text_encoding::EncodeExact(localWide.c_str(), CP_UTF8, expectedUtf8),
        "the real mapped target has an exact UTF-8 reference");
    const std::string nativeText = FullPathText(&::GetFullPathNameA, legacyBytes.c_str());
    const std::string wrapperText = FullPathText(&EffectFullPathNameA, legacyBytes.c_str());
    Check(!nativeText.empty() && wrapperText != nativeText,
        "an unencodable project target is not reported as the native legacy spelling");
    const std::wstring legacyWide = NormalizedWidePath(legacy);
    std::wstring decodedWrapper;
    Check(wrapperText == expectedUtf8 &&
        text_encoding::Decode(wrapperText.c_str(), CP_UTF8, decodedWrapper) &&
        _wcsicmp(decodedWrapper.c_str(), localWide.c_str()) == 0 &&
        _wcsicmp(decodedWrapper.c_str(), legacyWide.c_str()) != 0,
        "the result strictly decodes as UTF-8 to the actual project target");
    HANDLE opened = CreateFileW(decodedWrapper.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(opened != INVALID_HANDLE_VALUE, "the strict UTF-8 result opens the real project file");
    std::array<char, 64> actualBytes{}; DWORD actualRead = 0;
    Check(ReadFile(opened, actualBytes.data(), DWORD(actualBytes.size()), &actualRead, nullptr) != FALSE,
        "read the file named by the strict UTF-8 result");
    CloseHandle(opened);
    Check(std::string(actualBytes.data(), actualRead) == "acp-project" &&
        Bytes(legacy) == "acp-exe",
        "the strict UTF-8 result does not designate the same-named EXE file");
    CheckUtf8FullPathContract("unencodable local", legacyBytes.c_str(), local);
    PrepareEffectProjectPaths(nullptr);
}
// A relative EMM assignment can carry names the active ACP cannot spell. The
// original bytes must be decoded before any ANSI expansion; otherwise the
// stored UTF-8 bytes are replaced and the assignment identity is lost.
void FullPathNameUtf8SourceAndRecovery(const fs::path& root) {
    const auto savedCwd = fs::current_path();
    PrepareEffectProjectPaths(nullptr);
    const std::wstring name = L"\U0001F9CA\U00020000\U000010FF.fx";
    const fs::path relative = fs::path(L"effects") / name;
    std::string acpRelative;
    if (text_encoding::EncodeExact(relative.c_str(), GetACP(), acpRelative)) {
        std::printf("NOTE GetFullPathNameA UTF-8 source: ACP %u can encode the probe names; "
                    "the ANSI-extension regression cannot be constructed on this host.\n",
                    static_cast<unsigned>(GetACP()));
        return;
    }
    const auto fixture = root / (L"fullpath-utf8-source-" + std::to_wstring(GetCurrentProcessId()));
    const auto projectDirectory = fixture / L"project";
    const auto project = projectDirectory / L"scene.pmm";
    auto emm = project; emm.replace_extension(L".emm");
    const auto local = projectDirectory / relative;
    const auto legacy = root / relative;
    Write(local, "nonacp-project");
    Write(legacy, "nonacp-exe");
    std::string encodedRelative;
    Check(text_encoding::EncodeExact(relative.c_str(), CP_UTF8, encodedRelative),
        "encode the non-ACP relative assignment as strict UTF-8");
    // Do not assume every probe character makes the ACP reject these bytes:
    // U+1F9CA and U+20000 still decode as different ACP text on this host,
    // while the U+10FF spelling is the one the native decoder rejects. Record
    // the actual per-character outcome instead of encoding a guess as a fact.
    const wchar_t* const probes[] = {L"\U0001F9CA", L"\U00020000", L"\U000010FF"};
    const unsigned long probePoints[] = {0x1F9CAul, 0x20000ul, 0x10FFul};
    for (std::size_t index = 0; index < 3; ++index) {
        std::string probeBytes;
        Check(text_encoding::EncodeExact(probes[index], CP_UTF8, probeBytes),
            "encode a probe character as strict UTF-8");
        std::wstring reading;
        const bool readable = text_encoding::Decode(probeBytes.c_str(), GetACP(), reading);
        Check(!readable || _wcsicmp(reading.c_str(), probes[index]) != 0,
            "the ACP reading of a probe can never reproduce the real character");
        std::printf("NOTE GetFullPathNameA UTF-8 source: U+%04lX UTF-8 bytes under ACP %u are %s.\n",
            probePoints[index], static_cast<unsigned>(GetACP()),
            readable ? "decoded to a different name" : "rejected");
    }
    if (GetACP() == 936) {
        std::wstring rejected;
        Check(!text_encoding::Decode(encodedRelative.c_str(), GetACP(), rejected),
            "the full UTF-8 assignment name is rejected by ACP936 at its U+10FF bytes");
    } else {
        std::printf("NOTE GetFullPathNameA UTF-8 source: ACP %u is not 936; the U+10FF "
                    "rejection precondition is recorded but not exercised.\n",
                    static_cast<unsigned>(GetACP()));
    }
    Write(emm, "[Effect]\nPmd1=" + encodedRelative + "\n");
    PrepareEffectProjectPaths(project.c_str());
    std::string legacyUtf8;
    Check(text_encoding::EncodeExact(legacy.c_str(), CP_UTF8, legacyUtf8),
        "encode the legacy module-relative expansion as strict UTF-8");
    const std::string mappedContent = Read(legacyUtf8.c_str());
    Check(mappedContent == "nonacp-project" && mappedContent != "nonacp-exe" &&
        Bytes(legacy) == "nonacp-exe" && Bytes(local) == "nonacp-project",
        "the UTF-8 assignment source resolves the non-ACP project effect, not the EXE effect");
    std::wstring localWide;
    std::string expectedUtf8;
    Check(StrictUtf8Path(local, localWide, expectedUtf8),
        "the non-ACP project target has an exact UTF-8 reference");
    const std::wstring legacyWide = NormalizedWidePath(legacy);
    const std::string nativeText = FullPathText(&::GetFullPathNameA, legacyUtf8.c_str());
    const std::string wrapperText = FullPathText(&EffectFullPathNameA, legacyUtf8.c_str());
    std::wstring decodedWrapper;
    Check(wrapperText == expectedUtf8 &&
        text_encoding::Decode(wrapperText.c_str(), CP_UTF8, decodedWrapper) &&
        _wcsicmp(decodedWrapper.c_str(), localWide.c_str()) == 0 &&
        _wcsicmp(decodedWrapper.c_str(), legacyWide.c_str()) != 0 &&
        !SameFullPathText(nativeText, local),
        "non-ACP assignment names are decoded first and returned as the real UTF-8 path");
    CheckUtf8FullPathContract("non-acp assignment source", legacyUtf8.c_str(), local);
    // A restarted process has no assignment. Its persisted UTF-8 absolute path
    // must still open the same file, even when the process directory moved.
    const auto unrelated = fixture / L"unrelated-cwd";
    fs::create_directories(unrelated);
    fs::current_path(unrelated);
    PrepareEffectProjectPaths(nullptr);
    const std::string recovered = FullPathText(&EffectFullPathNameA, wrapperText.c_str());
    Check(!recovered.empty() && recovered == wrapperText,
        "a persisted UTF-8 absolute path survives removal of the EMM assignment");
    const std::string recoveredContent = Read(recovered.c_str());
    Check(recoveredContent == "nonacp-project" && recoveredContent != Bytes(legacy),
        "the recovered UTF-8 path reads the same project file without any mapping");
    CheckUtf8FullPathContract("persisted utf8 recovery", recovered.c_str(), local);
    // A relative UTF-8 source must normalize against the real process directory
    // rather than an inferred former EXE directory.
    const auto cwdRoot = fixture / L"cwd";
    const fs::path cwdRelative = fs::path(L"child") / L"\U0001F600-cwd.fx";
    const auto cwdFile = cwdRoot / cwdRelative;
    const auto exeGuess = root / cwdRelative;
    Write(cwdFile, "cwd-file");
    Write(exeGuess, "exe-guess");
    fs::current_path(cwdRoot);
    std::string cwdInput;
    Check(text_encoding::EncodeExact(cwdRelative.c_str(), CP_UTF8, cwdInput),
        "encode the relative UTF-8 source against the process directory");
    const std::string cwdWrapper = FullPathText(&EffectFullPathNameA, cwdInput.c_str());
    const std::wstring cwdFileWide = NormalizedWidePath(cwdFile);
    const std::wstring exeGuessWide = NormalizedWidePath(exeGuess);
    std::wstring cwdDecoded;
    Check(text_encoding::Decode(cwdWrapper.c_str(), CP_UTF8, cwdDecoded) &&
        _wcsicmp(cwdDecoded.c_str(), cwdFileWide.c_str()) == 0 &&
        _wcsicmp(cwdDecoded.c_str(), exeGuessWide.c_str()) != 0,
        "relative UTF-8 input normalizes against the real cwd, not the old EXE directory");
    Check(Read(cwdInput.c_str()) == "cwd-file" && Bytes(exeGuess) == "exe-guess",
        "cwd-relative resolution keeps the EXE-directory namesake untouched");
    CheckUtf8FullPathContract("relative utf8 cwd", cwdInput.c_str(), cwdFile);
    fs::current_path(savedCwd);
    PrepareEffectProjectPaths(nullptr);
}
// With no assignment, a strict UTF-8 spelling may only change the result when
// it names an existing regular file. Missing, directory, invalid and
// allocation-failure inputs keep the exact native ANSI behavior.
void FullPathNameDirectUtf8(const fs::path& root) {
    PrepareEffectProjectPaths(nullptr);
    const auto fixture = root / (L"fullpath-direct-" + std::to_wstring(GetCurrentProcessId()));
    const auto file = fixture / L"\U0001F600-direct.fx";
    Write(file, "direct-utf8");
    std::string fileBytes;
    Check(text_encoding::EncodeExact(file.c_str(), CP_UTF8, fileBytes),
        "encode the unmapped UTF-8 fixture as strict UTF-8");
    std::string acpFile;
    if (text_encoding::EncodeExact(file.c_str(), GetACP(), acpFile)) {
        std::printf("NOTE GetFullPathNameA direct UTF-8: ACP %u can spell the probe file; "
                    "the unmapped strict UTF-8 branch is not constructible on this host.\n",
                    static_cast<unsigned>(GetACP()));
    } else {
        Check(Read(fileBytes.c_str()) == "direct-utf8",
            "an unmapped strict UTF-8 spelling opens its existing regular file");
        std::wstring fileWide;
        std::string expectedUtf8;
        Check(StrictUtf8Path(file, fileWide, expectedUtf8),
            "the unmapped UTF-8 fixture has an exact UTF-8 reference");
        const std::string wrapperText = FullPathText(&EffectFullPathNameA, fileBytes.c_str());
        std::wstring decoded;
        Check(wrapperText == expectedUtf8 &&
            text_encoding::Decode(wrapperText.c_str(), CP_UTF8, decoded) &&
            _wcsicmp(decoded.c_str(), fileWide.c_str()) == 0,
            "an existing regular UTF-8 file keeps its real UTF-8 absolute path");
        CheckUtf8FullPathContract("unmapped utf8 file", fileBytes.c_str(), file);
    }
    const auto missing = fixture / L"\U0001F600-missing.fx";
    std::string missingBytes;
    Check(text_encoding::EncodeExact(missing.c_str(), CP_UTF8, missingBytes),
        "encode the unmapped UTF-8 missing fixture");
    Check(!fs::exists(missing), "the unmapped UTF-8 missing fixture stays absent");
    CheckFullPathContract("unmapped utf8 missing", missingBytes.c_str(), missingBytes.c_str());
    const auto directory = fixture / L"\U0001F600-directory.fx";
    fs::create_directories(directory);
    std::string directoryBytes;
    Check(text_encoding::EncodeExact(directory.c_str(), CP_UTF8, directoryBytes),
        "encode the unmapped UTF-8 directory fixture");
    CheckFullPathContract("unmapped utf8 directory", directoryBytes.c_str(), directoryBytes.c_str());
    std::string invalid;
    Check(text_encoding::EncodeExact(fixture.c_str(), CP_UTF8, invalid),
        "encode the invalid UTF-8 fixture prefix");
    invalid += "\\invalid-";
    invalid.push_back(static_cast<char>(0xF0));
    invalid += ".fx";
    std::wstring invalidDecoded;
    Check(!text_encoding::Decode(invalid.c_str(), CP_UTF8, invalidDecoded),
        "the invalid UTF-8 fixture input is really invalid");
    CheckFullPathContract("unmapped invalid utf8", invalid.c_str(), invalid.c_str());
    CheckFullPathContract("invalid null input", nullptr, nullptr);
    CheckFullPathContract("unmapped utf8 allocation fallback", fileBytes.c_str(),
        fileBytes.c_str(), true);
    Check(!failAllocation, "direct UTF-8 allocation fault injection is cleared");
    PrepareEffectProjectPaths(nullptr);
}
// The UTF-8 fallback must never claim bytes that already name an existing file
// under the active ACP. When the same bytes are valid in both encodings, the
// native ACP file wins even though another UTF-8 file has the same bytes.
void FullPathNameAcpSourcePriority(const fs::path& root) {
    PrepareEffectProjectPaths(nullptr);
    const std::string shared = "\xC3\xA9";
    std::wstring utf8Name, acpName;
    if (!text_encoding::Decode(shared.c_str(), CP_UTF8, utf8Name) ||
        !text_encoding::Decode(shared.c_str(), GetACP(), acpName) ||
        _wcsicmp(utf8Name.c_str(), acpName.c_str()) == 0) {
        std::printf("NOTE GetFullPathNameA ACP priority: ACP %u does not split the probe bytes "
                    "into two different names; the fixture cannot be constructed.\n",
                    static_cast<unsigned>(GetACP()));
        return;
    }
    const auto fileName = [](const std::wstring& value) {
        if (value.empty()) return false;
        for (const wchar_t character : value)
            if (character < 0x20 || character == L'\\' || character == L'/' ||
                character == L':' || character == L'*' || character == L'?' ||
                character == L'"' || character == L'<' || character == L'>' ||
                character == L'|')
                return false;
        return true;
    };
    std::string rootAcp, rootUtf8;
    if (!fileName(utf8Name) || !fileName(acpName) ||
        !text_encoding::EncodeExact(root.c_str(), GetACP(), rootAcp) ||
        !text_encoding::EncodeExact(root.c_str(), CP_UTF8, rootUtf8) ||
        rootAcp != rootUtf8) {
        std::printf("NOTE GetFullPathNameA ACP priority: ACP %u cannot build the two-file "
                    "fixture under this executable directory; the fact is recorded here.\n",
                    static_cast<unsigned>(GetACP()));
        return;
    }
    const auto fixture = root / (L"fullpath-acp-priority-" + std::to_wstring(GetCurrentProcessId()));
    const auto acpPath = fixture / (acpName + L".fx");
    const auto utf8Path = fixture / (utf8Name + L".fx");
    if (_wcsicmp(acpPath.c_str(), utf8Path.c_str()) == 0) {
        std::printf("NOTE GetFullPathNameA ACP priority: ACP %u resolves both interpretations "
                    "to one spelling; the fixture cannot be constructed.\n",
                    static_cast<unsigned>(GetACP()));
        return;
    }
    Write(acpPath, "acp-wins");
    Write(utf8Path, "utf8-loses");
    const std::string directoryBytes = rootAcp + "\\fullpath-acp-priority-" +
        std::to_string(GetCurrentProcessId());
    const std::string input = directoryBytes + "\\" + shared + ".fx";
    std::wstring acpDecoded, utf8Decoded;
    Check(text_encoding::Decode(input.c_str(), GetACP(), acpDecoded) &&
        _wcsicmp(fs::path(acpDecoded).lexically_normal().c_str(),
                 acpPath.lexically_normal().c_str()) == 0,
        "the shared bytes name the ACP fixture file");
    Check(text_encoding::Decode(input.c_str(), CP_UTF8, utf8Decoded) &&
        _wcsicmp(fs::path(utf8Decoded).lexically_normal().c_str(),
                 utf8Path.lexically_normal().c_str()) == 0,
        "the same shared bytes name a different UTF-8 fixture file");
    Check(Bytes(acpPath) == "acp-wins" && Bytes(utf8Path) == "utf8-loses",
        "the two real same-byte fixture files stay distinct");
    std::printf("NOTE GetFullPathNameA ACP priority: ACP %u constructs two real files from "
                "the same bytes; the ACP file must win.\n",
                static_cast<unsigned>(GetACP()));
    CheckFullPathContract("acp priority", input.c_str(), input.c_str());
    const std::string nativeText = FullPathText(&::GetFullPathNameA, input.c_str());
    const std::string wrapperText = FullPathText(&EffectFullPathNameA, input.c_str());
    Check(wrapperText == nativeText && SameFullPathText(wrapperText, acpPath) &&
        !SameFullPathText(wrapperText, utf8Path),
        "an existing ACP file takes priority over a different UTF-8 namesake");
    Check(Read(input.c_str()) == "acp-wins" && Bytes(utf8Path) == "utf8-loses",
        "the compatibility open also keeps the native ACP file instead of the namesake");
    PrepareEffectProjectPaths(nullptr);
}
void ImportScope() {
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(image != nullptr, "allocate original PE import fixture");
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image); dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x100;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + 0x100); nt->Signature = IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC; nt->OptionalHeader.SizeOfImage = 4096;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0x300, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + 0x300);
    descriptor->Name = 0x400; descriptor->OriginalFirstThunk = 0x500; descriptor->FirstThunk = 0x600;
    std::strcpy(reinterpret_cast<char*>(image + 0x400), "user32.dll");
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(image + 0x500); names[0].u1.AddressOfData = 0x700;
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(image + 0x600); slots[0].u1.Function = reinterpret_cast<ULONG_PTR>(&CreateFileA);
    std::strcpy(reinterpret_cast<char*>(image + 0x700 + sizeof(WORD)), "CreateFileA");
    PatchImports(reinterpret_cast<HMODULE>(image));
    Check(slots[0].u1.Function == reinterpret_cast<ULONG_PTR>(&CreateFileA), "unrelated library imports are not patched");
    std::strcpy(reinterpret_cast<char*>(image + 0x400), "kernel32.dll");
    PatchImports(reinterpret_cast<HMODULE>(image));
    Check(slots[0].u1.Function == reinterpret_cast<ULONG_PTR>(&OpenEffectFile), "declared native effect file import receives compatibility callback");
    PatchImports(reinterpret_cast<HMODULE>(image));
    Check(slots[0].u1.Function == reinterpret_cast<ULONG_PTR>(&OpenEffectFile), "repeated install preserves existing hook");
    // Extend this same import fixture: GetFullPathNameA is replaced only in a
    // kernel32 slot that still holds the system function. A same-named import
    // from another DLL and an existing third-party slot stay untouched.
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
        static_cast<DWORD>(3 * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    names[1].u1.AddressOfData = 0x720;
    slots[1].u1.Function = reinterpret_cast<ULONG_PTR>(&GetFullPathNameA);
    std::strcpy(reinterpret_cast<char*>(image + 0x720 + sizeof(WORD)), "GetFullPathNameA");
    auto* otherImport = descriptor + 1;
    otherImport->Name = 0x420; otherImport->OriginalFirstThunk = 0x540; otherImport->FirstThunk = 0x640;
    std::strcpy(reinterpret_cast<char*>(image + 0x420), "user32.dll");
    auto* otherNames = reinterpret_cast<IMAGE_THUNK_DATA*>(image + 0x540);
    auto* otherSlots = reinterpret_cast<IMAGE_THUNK_DATA*>(image + 0x640);
    otherNames[0].u1.AddressOfData = 0x750;
    otherSlots[0].u1.Function = reinterpret_cast<ULONG_PTR>(&GetFullPathNameA);
    std::strcpy(reinterpret_cast<char*>(image + 0x750 + sizeof(WORD)), "GetFullPathNameA");
    PatchImports(reinterpret_cast<HMODULE>(image));
    Check(slots[1].u1.Function == reinterpret_cast<ULONG_PTR>(&EffectFullPathNameA),
        "kernel32 GetFullPathNameA slot receives the compatibility callback");
    Check(otherSlots[0].u1.Function == reinterpret_cast<ULONG_PTR>(&GetFullPathNameA),
        "a same-named GetFullPathNameA import in another DLL is not patched");
    Check(slots[0].u1.Function == reinterpret_cast<ULONG_PTR>(&OpenEffectFile),
        "extending the fixture preserves the CreateFileA hook");
    PatchImports(reinterpret_cast<HMODULE>(image));
    Check(slots[1].u1.Function == reinterpret_cast<ULONG_PTR>(&EffectFullPathNameA) &&
        otherSlots[0].u1.Function == reinterpret_cast<ULONG_PTR>(&GetFullPathNameA),
        "repeated install preserves both GetFullPathNameA slots");
    slots[1].u1.Function = reinterpret_cast<ULONG_PTR>(&ThirdPartyFullPathNameA);
    PatchImports(reinterpret_cast<HMODULE>(image));
    Check(slots[1].u1.Function == reinterpret_cast<ULONG_PTR>(&ThirdPartyFullPathNameA),
        "an existing third-party GetFullPathNameA slot is never overwritten");
    VirtualFree(image, 0, MEM_RELEASE);
}
void ModuleFileNameForwarding() {
    Check(!plugins::LegacyMmeStatus().loaded, "unloaded legacy MME status drives module filename forwarding");
    const HMODULE modules[] = {nullptr, GetModuleHandleW(nullptr)};
    for (const HMODULE module : modules) {
        std::array<char, 32768> expected{}, forwarded{};
        const DWORD expectedLength = GetModuleFileNameA(module, expected.data(), DWORD(expected.size()));
        Check(expectedLength > 0 && expectedLength < expected.size(),
            "system module filename resolves the exact path into a large buffer");
        const DWORD forwardedLength = EffectModuleFileNameA(module, forwarded.data(), DWORD(forwarded.size()));
        Check(forwardedLength == expectedLength,
            "unloaded legacy MME forwards the system module filename return length");
        Check(std::memcmp(forwarded.data(), expected.data(), expectedLength + 1) == 0,
            "unloaded legacy MME forwards the exact system module filename path");
    }
}
}
static void ProjectRelocationPaths(const fs::path& root) {
    using namespace mikumikustudio;
    const auto old = root / L"prior-root", moved = root / L"moved-root";
    const auto original = old / L"Effects" / L"shared.fx";
    const auto restored = moved / L"Effects" / L"shared.fx";
    const auto relative = moved / L"Effects" / L"relative.fx";
    const auto project = old / L"Project" / L"sample.pmm";
    const auto emm = pmm_paths::Effect(project);
    const auto foreignProject = root / L"unrelated.pmm";
    const auto unassigned = old / L"Effects" / L"unassigned.fx";
    Write(restored, "confirmed effect");
    Write(relative, "confirmed relative effect");
    Write(moved / L"Effects" / L"unassigned.fx", "must not use this file");
    const std::string contents = "[Object]\nPmd1 = " + NativeBytes(unassigned) +
        "\n[Effect]\nPmd1 = " + NativeBytes(original) +
        "\n[Effect@MaterialMap]\nPmd1 = ..\\Effects\\relative.fx\n";
    Write(emm, contents);
    Write(pmm_paths::Effect(foreignProject), contents);

    auto roots = std::make_shared<project_relocation::RootMappings>();
    Check(roots->AddConfirmed((old / L"Models" / L"hero.pmx").native(),
                              (moved / L"Models" / L"hero.pmx").native()), "use the same root mapping as a successful model recovery");
    PrepareEffectProjectPaths(project.c_str());
    std::wstring resolved;
    Check(!RelocatedEffectPath(original.c_str(), resolved), "a parsed EMM alone cannot use tentative recovery mappings");
    PublishEffectProjectRelocation(project.c_str(), roots);
    Check(Read(NativeBytes(original).c_str()) == "confirmed effect", "the real read wrapper follows the EMM's relocated absolute effect");
    const auto legacyRelative = (root / L".." / L"Effects" / L"relative.fx").lexically_normal();
    Check(RelocatedEffectPath(legacyRelative.c_str(), resolved) && resolved == relative.native(),
          "the relative effect is anchored at its EMM before using the root mapping");
    Check(!RelocatedEffectPath(unassigned.c_str(), resolved), "an Object row cannot authorize arbitrary effect recovery");

    Write(original, "original recovered elsewhere");
    Check(Read(NativeBytes(original).c_str()) == "original recovered elsewhere", "an available original still wins over its relocated counterpart");
    Check(fs::remove(original), "remove only the owned original-effect fixture");
    PrepareEffectProjectPaths(foreignProject.c_str());
    Check(!RelocatedEffectPath(original.c_str(), resolved), "an unrelated EMM cannot use the current project's recovery mapping");
    PrepareEffectProjectPaths(project.c_str());
    roots.reset();
    Check(!RelocatedEffectPath(original.c_str(), resolved), "a retired project ticket cannot redirect delayed effect reads");
    Check(Bytes(emm) == contents, "effect recovery never rewrites the EMM source");
    PrepareEffectProjectPaths(nullptr);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = ExecutableDirectory(); const auto cwd = fs::current_path();
    Check(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"), "no third-party MME module loaded");
    ProjectSuffixPaths(root); ProjectNotificationAliases(root);
    AssignmentPaths(root); CompilerDiagnostics(root);
    FullPathNameCompatibility(root); FullPathNameLosslessEncoding(root);
    FullPathNameUtf8SourceAndRecovery(root); FullPathNameDirectUtf8(root);
    FullPathNameAcpSourcePriority(root);
    ProjectRelocationPaths(root);
    ImportScope(); ModuleFileNameForwarding();
    fs::current_path(cwd);
    Check(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"), "tests did not load optional third-party DLLs");
    std::printf("PASS effect path compatibility: %u checks\n", checks);
}
