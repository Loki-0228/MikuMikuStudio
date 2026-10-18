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
namespace mikudancestudio::runtime_log {
void Write(const char*, ...) noexcept { ++traces; SetLastError(8811); }
void WritePath(const char*, const wchar_t* path) noexcept {
    ++traces; wcscpy_s(tracedPath, path ? path : L""); SetLastError(8812);
}
}
namespace {
using namespace mikudancestudio;
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
    VirtualFree(image, 0, MEM_RELEASE);
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = ExecutableDirectory(); const auto cwd = fs::current_path();
    Check(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"), "no third-party MME module loaded");
    AssignmentPaths(root); CompilerDiagnostics(root); ImportScope(); fs::current_path(cwd);
    Check(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"), "tests did not load optional third-party DLLs");
    std::printf("PASS effect path compatibility: %u checks\n", checks);
}
