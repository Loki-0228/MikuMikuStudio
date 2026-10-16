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
}

namespace fs = std::filesystem;
namespace diagnostics = mikudancestudio::runtime_log;
namespace {
unsigned checks = 0;
bool blocked = false;
bool stress = false, writerFault = false;
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
int WINAPI HiddenMessage(HWND, LPCWSTR text, LPCWSTR, UINT) {
    const bool ok = text && (blocked ? wcsstr(text, L"could not save error log") :
        wcsstr(text, L"error log:") && wcsstr(text, L"\\backups\\error-"));
    if (!ok) TerminateProcess(GetCurrentProcess(), 251);
    if (stress || writerFault) {
        char snapshot[36000]{};
        unsigned count = 0;
        if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, snapshot, sizeof(snapshot), nullptr, nullptr) ||
            !ConsistentMessages(snapshot, count) || !count || recoveryCalls.load() != 1)
            TerminateProcess(GetCurrentProcess(), 250);
        // Parent verifies this marker so recursive termination cannot silently
        // pass merely because it happens to use the expected fatal exit code.
        HANDLE marker = CreateFileW(L"validated-dialog.ok", GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker == INVALID_HANDLE_VALUE) TerminateProcess(GetCurrentProcess(), 249);
        CloseHandle(marker);
    }
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
    diagnostics::Write("TEST_RECOVERY completed");
}
int Child(const std::wstring& mode) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    blocked = mode == L"blocked";
    stress = mode.find(L"stress-") == 0;
    writerFault = mode == L"writer-fault";
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
    if (mode == L"shutdown") diagnostics::BeginShutdown();
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
    for (const wchar_t* mode : {L"normal", L"seh", L"fatal", L"terminate", L"abort", L"shutdown", L"blocked",
        L"stress-0", L"stress-1", L"stress-2", L"writer-fault"}) {
        const auto directory = root / L"中文程序" / mode;
        const auto cwd = root / L"different-working-folder" / mode;
        fs::create_directories(directory); fs::create_directories(cwd);
        const auto child = directory / L"runtime_log_tests.exe";
        fs::copy_file(executable, child);
        if (std::wstring(mode) == L"blocked") std::ofstream(directory / L"backups") << "keep this file";
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
        const DWORD expected = value == L"normal" ? 0 :
            value == L"seh" || value == L"shutdown" || insideWrite ? EXCEPTION_ACCESS_VIOLATION : 0xE0000004;
        Check(code == expected, "child exits through expected production fatal path");
        Check(!fs::exists(cwd / L"backups"), "working directory never receives crash files");
        if (value == L"normal") { Check(!fs::exists(directory / L"backups"), "normal run creates no crash folder"); continue; }
        if (value == L"blocked") { Check(Bytes(directory / L"backups") == "keep this file", "failed log creation preserves existing file"); continue; }
        if (concurrent || insideWrite) Check(fs::exists(cwd / L"validated-dialog.ok"),
            "fault handler reaches a consistent dialog after concurrent or interrupted writes");
        unsigned logs = 0;
        for (const auto& entry : fs::directory_iterator(directory / L"backups")) {
            ++logs;
            Check(entry.path().extension() == L".log" && entry.path().filename().wstring().find(L"error-") == 0,
                "crash file uses timestamped error name");
            const auto data = Bytes(entry.path());
            Check(data.find(value == L"seh" || value == L"shutdown" || insideWrite ? "0xC0000005" : "0xE0000004") != std::string::npos,
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
            if (value != L"shutdown") Check(data.find("TEST_RECOVERY completed") != std::string::npos &&
                data.find("test crash phase") != std::string::npos, "backup result appended to flushed crash report");
            else Check(data.find("TEST_RECOVERY") == std::string::npos, "shutdown fault skips unsafe project recovery");
            if (value == L"seh" || value == L"shutdown") Check(data.find("target=0x1234") != std::string::npos &&
                data.find("#0 ") != std::string::npos, "SEH report includes memory access and module backtrace");
        }
        Check(logs == 1, "each fatal event creates one error report");
        std::printf("PASS case=%ls\n", mode);
    }
    std::printf("PASS runtime diagnostics checks=%u\n", checks);
}
