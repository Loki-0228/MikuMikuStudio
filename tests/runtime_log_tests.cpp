// Run fatal paths only in disposable child processes, with all dialogs hidden.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include "mikudancestudio/runtime_log.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <atomic>
#include <cstring>

#if !defined(MMS_RUNTIME_LOG_TESTS)
#error runtime_log_tests requires MMS_RUNTIME_LOG_TESTS for the in-flight crash probe
#endif
namespace mikudancestudio::runtime_log::testing {
void OnNextWriteClaimed(void (*probe)() noexcept) noexcept;
bool ReadMemory(void* destination, ULONG_PTR address, size_t size) noexcept;
}

namespace fs = std::filesystem;
namespace diagnostics = mikudancestudio::runtime_log;
namespace {
unsigned checks = 0;
bool blocked = false;
bool stress = false, writerFault = false, nullExecute = false;
bool shutdownMode = false, guardBoundary = false, mutateCallsite = false;
bool guardModule = false;
HMODULE protectedModule = nullptr;
unsigned char* codePage = nullptr;
unsigned char* guardPage = nullptr;
DWORD pageBytes = 0;
char dialogSnapshot[36000]{};
constexpr unsigned kStressWriters = 6;
std::atomic<bool> stopWriters{false};
std::atomic<bool> hotWriters{false};
std::atomic<unsigned> writerProgress[kStressWriters]{};
std::atomic<unsigned> recoveryCalls{0};
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::printf("FAIL %s (win32=%lu)\n", message, GetLastError()); std::exit(1); }
}
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void StressMessage(char (&message)[240], unsigned writer, unsigned number) noexcept {
    char fill[81];
    std::memset(fill, 'A' + writer, sizeof(fill) - 1);
    fill[sizeof(fill) - 1] = '\0';
    std::snprintf(message, sizeof(message), "STRESS W=%u N=%08u R=%08u F=%s E=%u/%08u END",
        writer, number, number, fill, writer, number);
}
bool ConsistentMessages(const char* text, unsigned& count) noexcept {
    count = 0;
    while (const char* line = std::strstr(text, "STRESS W=")) {
        unsigned writer = 0, number = 0;
        if (sscanf_s(line, "STRESS W=%u N=%u", &writer, &number) != 2 || writer > 7) return false;
        char expected[240]; StressMessage(expected, writer, number);
        const auto length = std::strlen(expected);
        if (std::strncmp(line, expected, length) ||
            (line[length] != '\r' && line[length] != '\n' && line[length] != '\0')) return false;
        ++count;
        text = line + length;
    }
    return true;
}
DWORD WINAPI StressWriter(void* parameter) {
    const auto writer = static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(parameter));
    for (unsigned number = 1; !stopWriters.load(std::memory_order_relaxed); ++number) {
        char message[240]; StressMessage(message, writer, number);
        diagnostics::Write("%s", message);
        writerProgress[writer].store(number, std::memory_order_relaxed);
        if (!(number % 8) && !hotWriters.load(std::memory_order_relaxed)) Sleep(1);
    }
    return 0;
}
void StartStressWriters() {
    for (unsigned writer = 0; writer < kStressWriters; ++writer) {
        HANDLE thread = CreateThread(nullptr, 0, StressWriter,
            reinterpret_cast<void*>(static_cast<ULONG_PTR>(writer)), 0, nullptr);
        Check(thread != nullptr, "start real concurrent log writer");
        CloseHandle(thread); // Disposable child exits through the fatal path.
    }
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
        bool ready = true;
        for (auto& progress : writerProgress) ready = ready && progress.load(std::memory_order_relaxed) >= 256;
        if (ready) break;
        Check(GetTickCount64() < deadline, "all writers wrap the production ring before the fault");
        Sleep(1);
    }
    unsigned before[kStressWriters]{};
    for (unsigned writer = 0; writer < kStressWriters; ++writer) before[writer] = writerProgress[writer].load();
    hotWriters.store(true);
    for (;;) {
        bool ready = true;
        for (unsigned writer = 0; writer < kStressWriters; ++writer)
            ready = ready && writerProgress[writer].load() > before[writer];
        if (ready) return;
        Check(GetTickCount64() < deadline, "all writers run continuously during the fault report");
        Sleep(1);
    }
}
void FaultWhileWriting() noexcept {
    // The outer Write owns a production slot. Reentrant writes wrap twice,
    // including attempts to reuse it, then the same thread faults inside Write.
    for (unsigned number = 1; number <= 400; ++number) {
        char message[240]; StressMessage(message, 7, number);
        diagnostics::Write("%s", message);
    }
    const ULONG_PTR details[] = {1, 0x5678};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, details);
}
void ExhaustStack(unsigned depth);
void (*volatile recurseStack)(unsigned) = ExhaustStack;
__declspec(noinline) void ExhaustStack(unsigned depth) {
    volatile unsigned char padding[4096];
    padding[depth % sizeof(padding)] = static_cast<unsigned char>(depth);
    recurseStack(depth + 1);
    diagnostics::Write("unreachable stack depth=%u byte=%u", depth, padding[depth % sizeof(padding)]);
}
int WINAPI HiddenMessage(HWND, LPCWSTR text, LPCWSTR, UINT) {
    if (shutdownMode) TerminateProcess(GetCurrentProcess(), 245);
    const bool ok = text && (blocked ? wcsstr(text, L"could not save error log") :
        wcsstr(text, L"error log:") && wcsstr(text, L"\\data\\backups\\error-"));
    if (!ok) TerminateProcess(GetCurrentProcess(), 251);
    if (nullExecute && (!wcsstr(text, L"fault context registers:") ||
        !wcsstr(text, L"null execute callsite:") || !wcsstr(text, L"bytes[-16,+8)")))
        TerminateProcess(GetCurrentProcess(), 247);
    if (codePage && !wcsstr(text, L"?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 31 C0 FF D0 | "))
        TerminateProcess(GetCurrentProcess(), 240);
    if (protectedModule) {
        wchar_t expected[128]{};
        _snwprintf_s(expected, _countof(expected), _TRUNCATE,
            L"base=%p image_size=unavailable timestamp=unavailable", protectedModule);
        if (!wcsstr(text, expected)) TerminateProcess(GetCurrentProcess(), 239);
    }
    if (stress || writerFault) {
        auto& snapshot = dialogSnapshot;
        unsigned count = 0;
        if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, snapshot, sizeof(snapshot), nullptr, nullptr) ||
            !ConsistentMessages(snapshot, count) || !count || recoveryCalls.load() != 1)
            TerminateProcess(GetCurrentProcess(), 250);
    }
    // Reaching the expected exit code with a partial log is not sufficient:
    // a second fault during report formatting can have the same exit code.
    HANDLE marker = CreateFileW(L"validated-dialog.ok", GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE) TerminateProcess(GetCurrentProcess(), 249);
    CloseHandle(marker);
    return IDOK;
}
void Recovery(unsigned long) noexcept {
    ++recoveryCalls;
    stopWriters.store(true, std::memory_order_relaxed);
    // No join/wait in the callback. Writers stop at their next loop check.
    // This stable tail also makes consistency assertions non-vacuous under
    // heavy disk contention, when the initial snapshot can legitimately skip.
    if (stress || writerFault) {
        const unsigned count = writerFault ? 400 : 16;
        for (unsigned number = 1; number <= count; ++number) {
            char message[240]; StressMessage(message, 6, 10000 + number);
            diagnostics::Write("%s", message);
        }
    }
    if (guardBoundary) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(guardPage, &region, sizeof(region)) || !(region.Protect & PAGE_GUARD))
            TerminateProcess(GetCurrentProcess(), 244);
        diagnostics::Write("TEST_GUARD remains armed");
    }
    if (guardModule) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(protectedModule, &region, sizeof(region)) || !(region.Protect & PAGE_GUARD))
            TerminateProcess(GetCurrentProcess(), 238);
        diagnostics::Write("TEST_MODULE_GUARD remains armed");
    }
    if (mutateCallsite) {
        DWORD previous = 0;
        if (!VirtualProtect(codePage, pageBytes, PAGE_READWRITE, &previous))
            TerminateProcess(GetCurrentProcess(), 243);
        std::memset(codePage, 0xCC, pageBytes);
        if (!VirtualProtect(codePage, pageBytes, PAGE_NOACCESS, &previous))
            TerminateProcess(GetCurrentProcess(), 242);
        diagnostics::Write("TEST_CALLSITE changed and made unreadable during recovery");
    }
    diagnostics::Write("TEST_RECOVERY completed");
}

void CheckMemoryReader() {
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto size = system.dwPageSize;
    auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, size * 3,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(pages != nullptr, "allocate diagnostic reader fixture");
    std::memset(pages, 0x5A, size * 3);
    unsigned char bytes[32]{};
    const auto start = reinterpret_cast<ULONG_PTR>(pages);
    Check(diagnostics::testing::ReadMemory(bytes, start, sizeof(bytes)) && bytes[0] == 0x5A,
        "diagnostic reader copies mapped bytes");
    Check(!diagnostics::testing::ReadMemory(bytes, 0, sizeof(bytes)) &&
        !diagnostics::testing::ReadMemory(bytes, start, 0) &&
        !diagnostics::testing::ReadMemory(bytes, ~ULONG_PTR{0} - 1, sizeof(bytes)),
        "diagnostic reader rejects null, empty and wrapping ranges");
    DWORD previous = 0;
    Check(VirtualProtect(pages + size, size, PAGE_NOACCESS, &previous) != FALSE,
        "protect unreadable reader page");
    Check(!diagnostics::testing::ReadMemory(bytes, start + size, sizeof(bytes)) &&
        !diagnostics::testing::ReadMemory(bytes, start + size - 1, sizeof(bytes)),
        "diagnostic reader rejects inaccessible and partially inaccessible ranges");
    Check(VirtualProtect(pages + size * 2, size, PAGE_READWRITE | PAGE_GUARD, &previous) != FALSE,
        "arm diagnostic reader guard page");
    Check(!diagnostics::testing::ReadMemory(bytes, start + size * 2, sizeof(bytes)),
        "diagnostic reader rejects a guard page without accessing it");
    MEMORY_BASIC_INFORMATION guard{};
    Check(VirtualQuery(pages + size * 2, &guard, sizeof(guard)) != 0 && (guard.Protect & PAGE_GUARD),
        "diagnostic reader does not consume a guard page");
    Check(VirtualProtect(pages, size, PAGE_EXECUTE, &previous) != FALSE &&
        !diagnostics::testing::ReadMemory(bytes, start, sizeof(bytes)),
        "diagnostic reader rejects execute-only memory");
    Check(VirtualFree(pages, 0, MEM_RELEASE) != FALSE &&
        !diagnostics::testing::ReadMemory(bytes, start, sizeof(bytes)),
        "diagnostic reader rejects released memory");
}

int Child(const std::wstring& mode) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    blocked = mode == L"blocked";
    stress = mode.find(L"stress-") == 0;
    writerFault = mode == L"writer-fault";
    nullExecute = mode.find(L"execute-null") == 0;
    shutdownMode = mode == L"shutdown" || mode == L"execute-null-shutdown";
    guardBoundary = mode == L"execute-null-guard";
    mutateCallsite = mode == L"execute-null-recovery";
    Check(MH_Initialize() == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&HiddenMessage), nullptr) == MH_OK &&
        MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "hide disposable child crash dialogs");
    diagnostics::Initialize(); diagnostics::SetCrashBackup(Recovery);
    if (mode == L"normal") { diagnostics::NormalExit(0); return 0; }
    for (int i = 0; i < 220; ++i) diagnostics::Write("history %d", i);
    diagnostics::WritePath("unicode", L"中文镜头.vmd");
    diagnostics::SetPhase("test crash phase");
    if (stress) {
        StartStressWriters();
        diagnostics::Fatal("concurrent writer fault");
    }
    if (writerFault) {
        diagnostics::testing::OnNextWriteClaimed(FaultWhileWriting);
        diagnostics::Write("fault while owning a ring slot");
        return 248;
    }
    if (mode == L"fatal" || blocked) diagnostics::Fatal("test fatal reason");
    if (mode == L"terminate") std::terminate();
    if (mode == L"abort") std::abort();
    if (mode == L"stack-overflow") ExhaustStack(0);
    if (shutdownMode) diagnostics::BeginShutdown();
    if (mode == L"execute-null" || mode == L"execute-null-shutdown") {
        auto missing = reinterpret_cast<void(*)()>(GetProcAddress(GetModuleHandleW(nullptr), "MissingCrashTestFunction"));
        missing();
        diagnostics::Write("unreachable after null call"); // Keep a real CALL, not a tail JMP.
    }
    if (mode == L"execute-null-boundary" || guardBoundary || mutateCallsite) {
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, system.dwPageSize * 2,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(memory != nullptr, "allocate isolated null-call code page");
        DWORD previous = 0;
        guardPage = memory;
        pageBytes = system.dwPageSize;
        Check(VirtualProtect(memory, system.dwPageSize,
            guardBoundary ? PAGE_READWRITE | PAGE_GUARD : PAGE_NOACCESS, &previous) != FALSE,
            "protect preceding code page from diagnostic reads");
        auto* code = memory + system.dwPageSize;
        codePage = code;
        const unsigned char instructions[] = {0x31, 0xC0, 0xFF, 0xD0, 0xC3}; // xor eax,eax; call rax/eax; ret
        std::memcpy(code, instructions, sizeof(instructions));
        Check(VirtualProtect(code, system.dwPageSize, PAGE_EXECUTE_READ, &previous) != FALSE &&
            FlushInstructionCache(GetCurrentProcess(), code, sizeof(instructions)) != FALSE,
            "publish isolated null-call code");
        reinterpret_cast<void(*)()>(code)();
        diagnostics::Write("unreachable after boundary null call");
    }
    if (mode == L"read-noaccess" || mode == L"write-noaccess" || mode == L"execute-nonzero") {
        auto* memory = static_cast<volatile unsigned char*>(VirtualAlloc(nullptr, 4096,
            MEM_RESERVE | MEM_COMMIT, mode == L"execute-nonzero" ? PAGE_READWRITE : PAGE_NOACCESS));
        Check(memory != nullptr, "allocate a real non-null fault target");
        if (mode == L"read-noaccess") diagnostics::Write("unreachable read=%u", *memory);
        else if (mode == L"write-noaccess") *memory = 1;
        else {
            reinterpret_cast<void(*)()>(const_cast<unsigned char*>(memory))();
            diagnostics::Write("unreachable after DEP execution fault");
        }
        return 241;
    }
    if (mode == L"in-page") {
        const ULONG_PTR details[] = {0, 0x56789, ERROR_IO_DEVICE};
        RaiseException(EXCEPTION_IN_PAGE_ERROR, EXCEPTION_NONCONTINUABLE, 3, details);
    }
    if (mode.find(L"unreadable-") == 0) {
        auto* memory = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        Check(memory != nullptr, "allocate unreadable exception metadata page");
        CONTEXT context{}; RtlCaptureContext(&context);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = 0xE1234567;
        EXCEPTION_POINTERS pointers{&record, &context};
        if (mode == L"unreadable-context") pointers.ContextRecord = static_cast<CONTEXT*>(memory);
        if (mode == L"unreadable-record") pointers.ExceptionRecord = static_cast<EXCEPTION_RECORD*>(memory);
        const auto filter = SetUnhandledExceptionFilter(nullptr);
        Check(filter != nullptr, "retrieve the installed production exception filter");
        SetUnhandledExceptionFilter(filter);
        Check(filter(mode == L"unreadable-pointers" ? static_cast<EXCEPTION_POINTERS*>(memory) : &pointers)
            == EXCEPTION_EXECUTE_HANDLER, "production filter handles unreadable exception metadata");
        TerminateProcess(GetCurrentProcess(), 0xE1234567);
    }
    if (mode.find(L"module-header-") == 0) {
        // Load the system's own D3D9 before inducing the fault. No graphics
        // device or third-party file is required, and the handler never loads it.
        wchar_t path[MAX_PATH]{};
        const auto length = GetSystemDirectoryW(path, _countof(path));
        Check(length && length < MAX_PATH - 11 && !wcscat_s(path, L"\\d3d9.dll"),
            "resolve the installed system D3D9 fixture");
        protectedModule = LoadLibraryW(path);
        Check(protectedModule != nullptr, "load a system DLL before protecting its image header");
        diagnostics::Write("TEST_PROTECTED_MODULE base=%p", protectedModule);
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        DWORD previous = 0;
        guardModule = mode == L"module-header-guard";
        if (mode == L"module-header-damaged") {
            Check(VirtualProtect(protectedModule, system.dwPageSize, PAGE_READWRITE, &previous) != FALSE,
                "make only the disposable child's mapped header writable");
            static_cast<unsigned char*>(static_cast<void*>(protectedModule))[0] = 0;
            DWORD ignored = 0;
            Check(VirtualProtect(protectedModule, system.dwPageSize, previous, &ignored) != FALSE,
                "restore DLL header protection after corrupting its DOS signature");
        } else Check(VirtualProtect(protectedModule, system.dwPageSize,
                guardModule ? PAGE_READONLY | PAGE_GUARD : PAGE_NOACCESS, &previous) != FALSE,
            "protect the loaded DLL header without unloading its mapped identity");
        const ULONG_PTR details[] = {0, 0x87654};
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, details);
    }
    const ULONG_PTR details[] = {1, 0x1234};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, details);
    return 252;
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::wstring(argv[1]) == L"--child") return Child(argv[2]);
    Check(argc == 2, "provide a fresh artifact directory");
    const auto root = fs::absolute(argv[1]);
    Check(!fs::exists(root), "artifact folder must not exist");
    wchar_t executable[32768]{};
    Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "resolve test executable");
    CheckMemoryReader();
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    char identity[100];
    std::snprintf(identity, sizeof(identity), "image_size=0x%lX timestamp=0x%08lX",
        nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp);
    for (const wchar_t* mode : {L"normal", L"seh", L"fatal", L"terminate", L"abort", L"shutdown", L"blocked",
        L"stress-0", L"stress-1", L"stress-2", L"writer-fault", L"execute-null", L"execute-null-boundary",
        L"execute-null-guard", L"execute-null-recovery", L"execute-null-shutdown",
        L"read-noaccess", L"write-noaccess", L"execute-nonzero", L"in-page",
        L"unreadable-context", L"unreadable-record", L"unreadable-pointers", L"stack-overflow",
        L"module-header-noaccess", L"module-header-guard", L"module-header-damaged"}) {
        const auto directory = root / L"中文程序" / mode;
        const auto cwd = root / L"different-working-folder" / mode;
        fs::create_directories(directory); fs::create_directories(cwd);
        const auto child = directory / L"runtime_log_tests.exe";
        fs::copy_file(executable, child);
        if (std::wstring(mode) == L"blocked") { fs::create_directories(directory / L"data"); std::ofstream(directory / L"data" / L"backups") << "keep this file"; }
        std::wstring command = L"\"" + child.wstring() + L"\" --child " + mode;
        STARTUPINFOW start{sizeof(start)}; start.dwFlags = STARTF_USESHOWWINDOW; start.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, cwd.c_str(), &start, &process) != FALSE, "start isolated crash child");
        const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
        if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 253);
        DWORD code = 0; GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        Check(wait == WAIT_OBJECT_0, "crash child exits without an interactive dialog");
        const std::wstring value(mode);
        const bool concurrent = value.find(L"stress-") == 0;
        const bool insideWrite = value == L"writer-fault";
        const bool nullCall = value.find(L"execute-null") == 0;
        const bool closing = value == L"shutdown" || value == L"execute-null-shutdown";
        const bool memoryFault = value == L"read-noaccess" || value == L"write-noaccess" || value == L"execute-nonzero";
        const bool unreadable = value.find(L"unreadable-") == 0;
        const bool protectedHeader = value.find(L"module-header-") == 0;
        const DWORD expected = value == L"normal" ? 0 :
            unreadable ? 0xE1234567 : value == L"in-page" ? EXCEPTION_IN_PAGE_ERROR :
            value == L"stack-overflow" ? EXCEPTION_STACK_OVERFLOW :
            value == L"seh" || closing || insideWrite || nullCall || memoryFault || protectedHeader ? EXCEPTION_ACCESS_VIOLATION : 0xE0000004;
        Check(code == expected, "child exits through expected production fatal path");
        Check(!fs::exists(cwd / L"data" / L"backups"), "working directory never receives crash files");
        if (value == L"normal") { Check(!fs::exists(directory / L"data" / L"backups"), "normal run creates no crash folder"); continue; }
        Check(fs::exists(cwd / L"validated-dialog.ok") != closing,
            "non-shutdown faults reach the validated dialog and shutdown faults stay silent");
        if (value == L"blocked") { Check(Bytes(directory / L"data" / L"backups") == "keep this file", "failed log creation preserves existing file"); continue; }
        if (concurrent || insideWrite) Check(fs::exists(cwd / L"validated-dialog.ok"),
            "fault handler reaches a consistent dialog after concurrent or interrupted writes");
        unsigned logs = 0;
        for (const auto& entry : fs::directory_iterator(directory / L"data" / L"backups")) {
            ++logs;
            Check(entry.path().extension() == L".log" && entry.path().filename().wstring().find(L"error-") == 0,
                "crash file uses timestamped error name");
            const auto data = Bytes(entry.path());
            char expectedCode[32];
            const DWORD recordedCode = value == L"unreadable-record" || value == L"unreadable-pointers" ? 0 : expected;
            std::snprintf(expectedCode, sizeof(expectedCode), "code: 0x%08lX", recordedCode);
            Check(data.find(expectedCode) != std::string::npos,
                "error log includes exception code");
            if (concurrent || insideWrite) {
                unsigned count = 0;
                Check(ConsistentMessages(data.c_str(), count) && count > 0,
                    "persisted concurrent records contain complete matching fields and payloads");
                if (insideWrite) Check(data.find("target=0x5678") != std::string::npos &&
                    data.find("N=00010400") != std::string::npos,
                    "fault inside an owned slot skips it and recovery wraps the ring again without waiting");
            } else {
                Check(data.find("中文镜头.vmd") != std::string::npos, "error log preserves UTF-8 paths");
                Check(data.find("history 100\r\n") != std::string::npos && data.find("history 219\r\n") != std::string::npos,
                    "log contains retained ring history beyond the dialog tail");
            }
            Check(data.find("build=") != std::string::npos && data.find("image_base=") != std::string::npos,
                "report includes build and loaded image address after ring wraps");
            Check(data.find("loaded graphics modules:") != std::string::npos &&
                data.find("image_size=0x") != std::string::npos && data.find("timestamp=0x") != std::string::npos,
                "report retains loaded module identity independently of ring history");
            const auto module = data.find("runtime_log_tests.exe base=");
            Check(module != std::string::npos &&
                data.substr(module, data.find('\n', module) - module).find(identity) != std::string::npos,
                "reported executable timestamp and image size match the copied binary");
            if (!closing) Check(data.find("TEST_RECOVERY completed") != std::string::npos &&
                data.find("test crash phase") != std::string::npos, "backup result appended to flushed crash report");
            else Check(data.find("TEST_RECOVERY") == std::string::npos, "shutdown fault skips unsafe project recovery");
            if (value == L"seh" || value == L"shutdown") Check(data.find("target=0x1234") != std::string::npos &&
                data.find("#0 ") != std::string::npos, "SEH report includes memory access and module backtrace");
            if (nullCall) {
                Check(data.find("target=0x0") != std::string::npos &&
                    data.find("fault context registers:") != std::string::npos &&
#if defined(_M_X64)
                    data.find("RIP=0000000000000000") != std::string::npos &&
                    data.find("RAX=") != std::string::npos && data.find("RCX=") != std::string::npos &&
                    data.find("R15=") != std::string::npos,
#else
                    data.find("EIP=00000000") != std::string::npos &&
                    data.find("EAX=") != std::string::npos && data.find("ECX=") != std::string::npos,
#endif
                    "null execute logs retain the original instruction pointer and register context");
                const auto callsite = data.find("null execute callsite: stack_return=");
                const auto bytes = data.find("bytes[-16,+8) (| = return address; ?? = unreadable): ", callsite);
                Check(callsite != std::string::npos && bytes != std::string::npos,
                    "null execute logs retain bounded raw bytes at the stack return candidate");
                const auto payload = data.substr(bytes + std::strlen(
                    "bytes[-16,+8) (| = return address; ?? = unreadable): "), 74);
                Check(payload.size() == 74 && payload.substr(48, 2) == "| ",
                    "callsite bytes contain exactly sixteen bytes before and eight after the marked return");
                if (value == L"execute-null" || value == L"execute-null-shutdown") {
                    Check(data.find("runtime_log_tests.exe+", callsite) < bytes &&
                        payload.find("??") == std::string::npos,
                        "readable real callsite includes its module offset and all twenty-four raw bytes");
#if defined(_M_X64)
                    Check(data.find("#1 runtime_log_tests.exe+") != std::string::npos,
                        "null execute reports preserve the caller's module and offset");
#endif
                } else Check(payload.substr(0, 36) == "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? " &&
                    payload.substr(36, 12) == "31 C0 FF D0 ",
                    "unreadable bytes before the executable page are marked without losing readable call bytes");
            }
            if (value == L"execute-null-guard") Check(data.find("TEST_GUARD remains armed") != std::string::npos,
                "callsite sampling preserves the protection of an adjacent guard page");
            if (value == L"execute-null-recovery") Check(data.find("TEST_CALLSITE changed and made unreadable") != std::string::npos,
                "the original callsite is retained when recovery replaces and protects its code");
            if (memoryFault || value == L"in-page") {
                const char* operation = value == L"write-noaccess" ? "写入 / write" :
                    value == L"execute-nonzero" ? "执行 / execute" : "读取 / read";
                Check(data.find(operation) != std::string::npos &&
                    data.find("fault context registers:") != std::string::npos &&
                    data.find("null execute callsite:") == std::string::npos,
                    "non-null read, write, execute and in-page faults do not invent a null callsite");
                Check(data.find("target=0x0") == std::string::npos &&
                    (value != L"in-page" || data.find("target=0x56789") != std::string::npos),
                    "non-null fault addresses survive diagnostic capture");
            }
            if (value == L"unreadable-context" || value == L"unreadable-pointers")
                Check(data.find("fault context: unreadable") != std::string::npos,
                    "unreadable context metadata is reported without dereferencing it");
            if (protectedHeader) {
                const char* marker = "TEST_PROTECTED_MODULE base=";
                const auto marked = data.find(marker);
                Check(marked != std::string::npos, "fixture records its loaded module handle before the fault");
                const auto address = marked + std::strlen(marker);
                const auto base = data.substr(address, data.find('\r', address) - address);
                Check(data.find("d3d9.dll base=" + base +
                    " image_size=unavailable timestamp=unavailable") != std::string::npos,
                    "an unreadable or damaged loaded image retains its path and base with explicit unavailable identity fields");
                if (value == L"module-header-guard")
                    Check(data.find("TEST_MODULE_GUARD remains armed") != std::string::npos,
                        "diagnostics never consume a guard protecting a loaded DLL header");
            }
        }
        Check(logs == 1, "each fatal event creates one error report");
        std::printf("PASS case=%ls\n", mode);
    }
    std::printf("PASS runtime diagnostics checks=%u\n", checks);
}
