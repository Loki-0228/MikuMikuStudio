// ===========================================================================
// MikuDanceStudio - runtime diagnostics and crash reports
// ===========================================================================
// Normal diagnostics stay in a bounded in-memory ring. Anything that would
// kill the process (unhandled exception, std::terminate, abort, invalid
// parameter, failed startup) saves an error log beside the EXE in backups
// and turns that ring into a modal "last error" window.
// MessageBoxW blocks the failing thread, so the process stays alive until the
// user presses OK - a silent flash-exit becomes a readable error report.
//
// Fault-handler constraints:
//   * diagnostics must not allocate, wait for a user-space lock or dereference
//     application state; a busy ring slot is skipped after one atomic attempt,
//     and every buffer is static or on the stack;
//   * the phase is a string literal only (see runtime_log.hpp);
//   * a faulting thread that already reported is not reported twice.
// Project recovery installs a separate, guarded best-effort callback.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>   // _ReturnAddress

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <cwchar>
#include <exception>

#include "mikudancestudio/runtime_log.hpp"

namespace mikudancestudio::runtime_log {
namespace {

// ---- in-memory record -----------------------------------------------------
constexpr int kRingLines = 192;    // ~60 KiB of history
constexpr int kLineBytes = 320;
constexpr int kDialogLines = 48;   // newest entries rendered into the dialog
constexpr int kDialogChars = 12000;

struct RingLine {
    // Both writers and snapshot readers claim once, without waiting. A fault
    // while owning a slot loses that slot; it cannot stall the crash handler.
    std::atomic_flag occupied = ATOMIC_FLAG_INIT;
    unsigned long long sequence = 0;
    unsigned int length = 0;
    char text[kLineBytes];
};

RingLine ring[kRingLines];
std::atomic<unsigned long long> nextSequence{1};
std::atomic<const char*> phase{"startup"};
std::atomic<bool> reporting{false};
std::atomic<bool> shuttingDown{false};
std::atomic<CrashBackup> crashBackup{nullptr};
ULONGLONG lastHeartbeat = 0;
wchar_t dialogText[kDialogChars];
wchar_t crashDirectory[32768];
wchar_t errorLogPath[32768];
char reportUtf8[kDialogChars * 3];
DWORD errorLogFailure = ERROR_SUCCESS;
unsigned long long savedSequence = 1;
#if defined(MMS_RUNTIME_LOG_TESTS)
using WriteClaimProbe = void (*)() noexcept;
std::atomic<WriteClaimProbe> writeClaimProbe{nullptr};
#endif

void PrepareCrashDirectory() noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, crashDirectory, _countof(crashDirectory));
    if (!length || length >= _countof(crashDirectory)) {
        crashDirectory[0] = L'\0';
        errorLogFailure = ERROR_FILENAME_EXCED_RANGE;
        return;
    }
    wchar_t* separator = std::wcsrchr(crashDirectory, L'\\');
    if (!separator || wcscpy_s(separator + 1,
        _countof(crashDirectory) - (separator + 1 - crashDirectory), L"backups")) {
        crashDirectory[0] = L'\0';
        errorLogFailure = ERROR_BAD_PATHNAME;
    }
}

void Append(const char* text, size_t size) noexcept {
    if (text == nullptr || size == 0) return;
    if (size > kLineBytes - 1) size = kLineBytes - 1;
    const unsigned long long sequence =
        nextSequence.fetch_add(1, std::memory_order_relaxed);
    RingLine& line = ring[sequence % kRingLines];
    if (line.occupied.test_and_set(std::memory_order_acquire)) return;
    if (line.sequence >= sequence) {
        line.occupied.clear(std::memory_order_release);
        return; // A delayed writer must not replace a newer completed wrap.
    }
#if defined(MMS_RUNTIME_LOG_TESTS)
    if (auto probe = writeClaimProbe.exchange(nullptr)) probe();
#endif
    std::memcpy(line.text, text, size);
    line.text[size] = '\0';
    line.length = static_cast<unsigned int>(size);
    line.sequence = sequence;
    line.occupied.clear(std::memory_order_release);
}

// Copy while owning the slot, then release it before formatting or disk I/O.
// Sequence checks alone would not make concurrent reads of plain text legal.
unsigned CopyLine(unsigned long long sequence, char (&copy)[kLineBytes]) noexcept {
    RingLine& line = ring[sequence % kRingLines];
    if (line.occupied.test_and_set(std::memory_order_acquire)) return 0;
    unsigned length = 0;
    if (line.sequence == sequence && line.length > 0 && line.length < kLineBytes) {
        length = line.length;
        std::memcpy(copy, line.text, length);
        copy[length] = '\0';
    }
    line.occupied.clear(std::memory_order_release);
    return length;
}

// UTC "HH:MM:SS.mmm [tid=N] message".
void Format(char* buffer, size_t capacity, const char* format, va_list args) noexcept {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    const int prefix = std::snprintf(buffer, capacity,
        "%02u:%02u:%02u.%03u [tid=%lu] ",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentThreadId());
    if (prefix <= 0 || static_cast<size_t>(prefix) >= capacity) {
        buffer[0] = '\0';
        return;
    }
    std::vsnprintf(buffer + prefix, capacity - static_cast<size_t>(prefix),
                   format, args);
    buffer[capacity - 1] = '\0';
}

void Record(const char* format, va_list args) noexcept {
    char buffer[kLineBytes];
    Format(buffer, sizeof(buffer), format, args);
    Append(buffer, std::strlen(buffer));
}

// ---- dialog rendering -----------------------------------------------------
void AppendText(int& used, const wchar_t* format, ...) noexcept {
    if (used < 0 || used >= kDialogChars - 1) {
        used = kDialogChars - 1;
        return;
    }
    va_list args;
    va_start(args, format);
    const int written = _vsnwprintf_s(dialogText + used,
        static_cast<size_t>(kDialogChars - used), _TRUNCATE, format, args);
    va_end(args);
    if (written < 0) {                       // truncated: buffer is terminated
        used = kDialogChars - 1;
        return;
    }
    used += written;
    dialogText[used] = L'\0';
}

// Narrow strings inside the report are UTF-8 (they can carry Chinese text);
// never print them through the ANSI "%hs" path.
const wchar_t* Widen(const char* utf8, wchar_t* buffer, int capacity) noexcept {
    if (utf8 == nullptr) utf8 = "";
    const int converted = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buffer, capacity);
    if (converted <= 0) buffer[0] = L'\0';
    return buffer;
}

const wchar_t* ExceptionName(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return L"访问冲突 / access violation";
    case EXCEPTION_IN_PAGE_ERROR:         return L"分页错误 / in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return L"非法指令 / illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:      return L"特权指令 / privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return L"整数除零 / integer divide by zero";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return L"浮点除零 / float divide by zero";
    case EXCEPTION_STACK_OVERFLOW:        return L"栈溢出 / stack overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return L"对齐错误 / misaligned access";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return L"数组越界 / array bounds exceeded";
    case 0xC0000374:                      return L"堆损坏 / heap corruption";
    case 0xC0000409:                      return L"栈缓冲区溢出 / stack buffer overrun";
    case 0xC0000602:                      return L"fail-fast 异常 / fail-fast exception";
    case 0xE0000004:                      return L"应用检测到致命状态 / fatal application state";
    default:                              return L"未知 / unknown";
    }
}

const wchar_t* AccessName(ULONG_PTR operation) noexcept {
    switch (operation) {
    case 0: return L"读取 / read";
    case 1: return L"写入 / write";
    case 8: return L"执行 / execute";
    default: return L"其他 / other";
    }
}

// Walks the ring oldest-first so the dialog reads like the tail of a log.
void AppendRecentLines(int& used) noexcept {
    wchar_t wide[kLineBytes];
    const unsigned long long next = nextSequence.load(std::memory_order_acquire);
    const unsigned long long first =
        next > static_cast<unsigned long long>(kDialogLines)
            ? next - static_cast<unsigned long long>(kDialogLines)
            : 1ULL;
    for (unsigned long long sequence = first; sequence < next; ++sequence) {
        char copy[kLineBytes];
        const unsigned length = CopyLine(sequence, copy);
        if (!length) continue;
        const int converted = MultiByteToWideChar(CP_UTF8, 0, copy,
            static_cast<int>(length), wide, kLineBytes - 1);
        if (converted <= 0) continue;
        wide[converted] = L'\0';
        AppendText(used, L"%ls\r\n", wide);
        if (used >= kDialogChars - 1) return;
    }
}

// ---- faulting backtrace ---------------------------------------------------
// A fault inside a system DLL (d3d9.dll above all) names only that module in
// the exception record; the callers that made the fatal call are the part the
// report actually needs.  The chain is therefore unwound from the faulting
// context with the kernel-provided table walker, which reads the already
// mapped .pdata of every image: no allocation, no symbol handler, no file.
constexpr int kBacktraceFrames = 24;
void* backtraceFrames[kBacktraceFrames];
int backtraceCount = 0;

void CaptureBacktrace(const CONTEXT* context) noexcept {
#if defined(_M_X64)
    backtraceCount = 0;
    if (context == nullptr) return;
    CONTEXT current = *context;
    backtraceFrames[backtraceCount++] = reinterpret_cast<void*>(current.Rip);
    while (backtraceCount < kBacktraceFrames && current.Rip != 0) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION entry =
            RtlLookupFunctionEntry(current.Rip, &imageBase, nullptr);
        if (entry == nullptr) {
            // Leaf function: no unwind data, the return address is on top of
            // the stack.  The stack being walked is the one this handler is
            // already running on, so the read stays inside a mapped page.
            const DWORD64* stack = reinterpret_cast<const DWORD64*>(current.Rsp);
            if (stack == nullptr) break;
            current.Rip = *stack;
            current.Rsp += sizeof(DWORD64);
        } else {
            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, current.Rip, entry,
                             &current, &handlerData, &establisherFrame,
                             nullptr);
        }
        if (current.Rip == 0) break;
        backtraceFrames[backtraceCount++] =
            reinterpret_cast<void*>(current.Rip);
    }
#endif
}

void CaptureBacktraceSafely(const CONTEXT* context) noexcept {
    __try { CaptureBacktrace(context); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Write("BACKTRACE stopped at an unreadable frame"); }
}

// "module.dll+0x1234" for an address inside a mapped image, else the address.
void FormatFrame(wchar_t* buffer, int capacity, const void* address) noexcept {
    buffer[0] = L'\0';
    const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(address);
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(value), &module) &&
        module != nullptr) {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < length; ++i) {
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        }
        _snwprintf_s(buffer, static_cast<size_t>(capacity), _TRUNCATE,
                     L"%ls+0x%llX", name,
                     static_cast<unsigned long long>(
                         value - reinterpret_cast<ULONG_PTR>(module)));
        return;
    }
    _snwprintf_s(buffer, static_cast<size_t>(capacity), _TRUNCATE, L"%p",
                 address);
}

int BuildReport(const char* reason, const EXCEPTION_RECORD* record) noexcept {
    wchar_t narrow[512];
    int used = 0;
    dialogText[0] = L'\0';
    AppendText(used,
        L"MikuDanceStudio 遇到致命错误，点击\u201c确定\u201d后程序才会关闭。\r\n"
        L"MikuDanceStudio hit a fatal error; the program closes only after OK.\r\n\r\n");
    if (reason != nullptr && *reason != '\0')
        AppendText(used, L"原因 / reason: %ls\r\n",
                   Widen(reason, narrow, _countof(narrow)));
    if (record != nullptr) {
        AppendText(used, L"错误代码 / code: 0x%08lX  %ls\r\n",
                   static_cast<unsigned long>(record->ExceptionCode),
                   ExceptionName(record->ExceptionCode));
        AppendText(used, L"错误地址 / address: %p\r\n", record->ExceptionAddress);
        MEMORY_BASIC_INFORMATION region{};
        if (record->ExceptionAddress != nullptr &&
            VirtualQuery(record->ExceptionAddress, &region, sizeof(region))) {
            AppendText(used, L"所属模块 / module: base=%p offset=0x%llX\r\n",
                region.AllocationBase,
                static_cast<unsigned long long>(
                    reinterpret_cast<ULONG_PTR>(record->ExceptionAddress) -
                    reinterpret_cast<ULONG_PTR>(region.AllocationBase)));
        }
        if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
             record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            record->NumberParameters >= 2) {
            AppendText(used, L"访问类型 / access: %ls target=0x%llX\r\n",
                AccessName(record->ExceptionInformation[0]),
                static_cast<unsigned long long>(record->ExceptionInformation[1]));
        }
    }
    AppendText(used, L"阶段 / phase: %ls\r\n",
               Widen(phase.load(), narrow, _countof(narrow)));
    AppendText(used, L"线程 / thread: %lu\r\n", GetCurrentThreadId());
    AppendText(used, L"进程 / process: %lu  arch=%u build=%hs %hs image_base=%p\r\n",
        GetCurrentProcessId(), static_cast<unsigned>(sizeof(void*) * 8),
        __DATE__, __TIME__, GetModuleHandleW(nullptr));
    if (backtraceCount > 0) {
        AppendText(used, L"调用栈 / backtrace (module+offset):\r\n");
        for (int i = 0; i < backtraceCount; ++i) {
            wchar_t frame[320];
            FormatFrame(frame, _countof(frame), backtraceFrames[i]);
            AppendText(used, L"  #%d %ls\r\n", i, frame);
        }
    }
    return used;
}

bool WriteBytes(HANDLE file, const char* bytes, DWORD size) noexcept {
    DWORD written = 0;
    if (!WriteFile(file, bytes, size, &written, nullptr) || written != size) {
        errorLogFailure = GetLastError();
        if (!errorLogFailure) errorLogFailure = ERROR_WRITE_FAULT;
        return false;
    }
    return true;
}

bool WriteHistory(HANDLE file, unsigned long long first) noexcept {
    const auto next = nextSequence.load(std::memory_order_acquire);
    const auto oldest = next > kRingLines ? next - kRingLines : 1ULL;
    if (first < oldest) first = oldest;
    for (auto sequence = first; sequence < next; ++sequence) {
        char copy[kLineBytes];
        const unsigned length = CopyLine(sequence, copy);
        if (!length) continue;
        if (!WriteBytes(file, copy, length) || !WriteBytes(file, "\r\n", 2)) return false;
    }
    savedSequence = next;
    return true;
}

void SaveErrorLog(const char* reason, const EXCEPTION_RECORD* record) noexcept {
    if (!*crashDirectory) return;
    if (!CreateDirectoryW(crashDirectory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        errorLogFailure = GetLastError();
        return;
    }
    SYSTEMTIME now{}; GetLocalTime(&now);
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned collision = 0; collision < 32; ++collision) {
        if (_snwprintf_s(errorLogPath, _countof(errorLogPath), _TRUNCATE,
            L"%ls\\error-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%u.log", crashDirectory,
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, GetCurrentProcessId(), collision) < 0) {
            errorLogFailure = ERROR_FILENAME_EXCED_RANGE;
            break;
        }
        file = CreateFileW(errorLogPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        errorLogFailure = GetLastError();
        if (errorLogFailure != ERROR_FILE_EXISTS && errorLogFailure != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) { errorLogPath[0] = L'\0'; return; }
    errorLogFailure = ERROR_SUCCESS;
    const int used = BuildReport(reason, record);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, dialogText, used,
        reportUtf8, sizeof(reportUtf8), nullptr, nullptr);
    bool ok = bytes > 0 && WriteBytes(file, reportUtf8, bytes) &&
        WriteBytes(file, "\r\nLast messages (UTC):\r\n", sizeof("\r\nLast messages (UTC):\r\n") - 1) && WriteHistory(file, 1);
    if (!FlushFileBuffers(file) && !errorLogFailure) errorLogFailure = GetLastError();
    CloseHandle(file);
    if (!ok && !errorLogFailure) errorLogFailure = ERROR_WRITE_FAULT;
}

void SaveRecoveryResult() noexcept {
    if (!*errorLogPath || errorLogFailure) return;
    HANDLE file = CreateFileW(errorLogPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) { errorLogFailure = GetLastError(); return; }
    WriteHistory(file, savedSequence);
    if (!FlushFileBuffers(file) && !errorLogFailure) errorLogFailure = GetLastError();
    CloseHandle(file);
}

void ShowLastError(const char* reason, const EXCEPTION_RECORD* record) noexcept {
    int used = BuildReport(reason, record);
    if (*errorLogPath && !errorLogFailure)
        AppendText(used, L"\r\n错误日志 / error log: %ls\r\n", errorLogPath);
    else AppendText(used, L"\r\n无法保存错误日志 / could not save error log (Win32=%lu)\r\n", errorLogFailure);
    AppendText(used, L"最后记录 / last messages:\r\n");
    AppendRecentLines(used);
    AppendText(used, L"\r\n(Ctrl+C 复制以上信息 / Ctrl+C copies this text)\r\n");

    const int result = MessageBoxW(nullptr, dialogText,
        L"MikuDanceStudio - 致命错误 / Fatal error",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST | MB_TASKMODAL);
    if (result == 0) {
        // The box could not be created (no interactive desktop, exhausted
        // desktop heap).  Retry with the minimal flag set instead of losing
        // the report and letting the process vanish.
        MessageBoxW(nullptr, dialogText,
                    L"MikuDanceStudio - 致命错误 / Fatal error",
                    MB_OK | MB_ICONERROR);
    }
}

[[noreturn]] void Fail(const char* reason, DWORD code, void* address) noexcept {
    if (!reporting.exchange(true)) {
        Write("FATAL %s at %p phase=%s", reason, address,
              phase.load() != nullptr ? phase.load() : "unknown");
        EXCEPTION_RECORD record{};
        record.ExceptionCode = code;
        record.ExceptionAddress = address;
        SaveErrorLog(reason, &record);
        if (!shuttingDown.load(std::memory_order_relaxed))
            if (auto backup = crashBackup.load()) backup(code);
        SaveRecoveryResult();
        // Teardown faults (after the close was confirmed) stay silent: the
        // settings are saved and the window is gone, so a modal error box
        // would only look like the program failed to close.
        if (!shuttingDown.load(std::memory_order_relaxed))
            ShowLastError(reason, &record);
    }
    // Reached after the user acknowledged the report (or directly when a
    // second fault arrived while the first report was still up).
    TerminateProcess(GetCurrentProcess(), code);
    std::_Exit(static_cast<int>(code));
}

void OnTerminate() noexcept {
    Fail("std::terminate (未捕获异常 / uncaught exception)", 0xE0000004, _ReturnAddress());
}
void OnAbort(int) {
    Fail("abort (SIGABRT)", 0xE0000004, _ReturnAddress());
}
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    Fail("CRT invalid parameter (无效参数)", 0xE0000004, _ReturnAddress());
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* pointers) noexcept {
    if (reporting.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    EXCEPTION_RECORD record{};
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
        record = *pointers->ExceptionRecord;
    record.ExceptionRecord = nullptr;
    Write("CRASH code=0x%08lX address=%p phase=%s thread=%lu",
          static_cast<unsigned long>(record.ExceptionCode),
          record.ExceptionAddress,
          phase.load() != nullptr ? phase.load() : "unknown", GetCurrentThreadId());
    if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2) {
        Write("ACCESS operation=%llu target=0x%llX (0=read,1=write,8=execute)",
              static_cast<unsigned long long>(record.ExceptionInformation[0]),
              static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }
    // Same teardown rule as Fail(): a fault while the program is already
    // closing is recorded, not reported.
    if (pointers != nullptr && pointers->ContextRecord != nullptr)
        CaptureBacktraceSafely(pointers->ContextRecord);
    SaveErrorLog(nullptr, &record);
    if (!shuttingDown.load(std::memory_order_relaxed))
        if (auto backup = crashBackup.load()) backup(record.ExceptionCode);
    SaveRecoveryResult();
    if (!shuttingDown.load(std::memory_order_relaxed))
        ShowLastError(nullptr, &record);
    // Returning EXCEPTION_EXECUTE_HANDLER lets the system finish the process
    // now that the user has seen (and can copy) the report.
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

#if defined(MMS_RUNTIME_LOG_TESTS)
namespace testing {
// Test-target-only seam. It is absent from the application and public header.
void OnNextWriteClaimed(void (*probe)() noexcept) noexcept { writeClaimProbe.store(probe); }
}
#endif

void SetCrashBackup(CrashBackup callback) noexcept { crashBackup.store(callback); }

void Initialize() noexcept {
    // Resolve the real EXE before a crash, without creating a directory or file.
    PrepareCrashDirectory();
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
    SetUnhandledExceptionFilter(OnCrash);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParameter);
    std::signal(SIGABRT, OnAbort);
    lastHeartbeat = GetTickCount64();
    Write("START pid=%lu arch=%u build=%s %s image_base=%p",
          GetCurrentProcessId(), static_cast<unsigned>(sizeof(void*) * 8),
          __DATE__, __TIME__, GetModuleHandleW(nullptr));
    wchar_t executable[32768]{};
    if (GetModuleFileNameW(nullptr, executable, _countof(executable)))
        WritePath("executable", executable);
    wchar_t directory[32768]{};
    if (GetCurrentDirectoryW(_countof(directory), directory))
        WritePath("working_directory", directory);
}

void Write(const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    Record(format, args);
    va_end(args);
}

void WritePath(const char* label, const wchar_t* path) noexcept {
    char utf8[1536]{};
    if (path && WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr))
        Write("%s=%s", label, utf8);
    else Write("%s=(unavailable or too long)", label);
}

void SetPhase(const char* value) noexcept { phase.store(value, std::memory_order_relaxed); }

void TraceV(const char* format, va_list args) noexcept {
    // Opt-in outside fatal errors. The guards call
    // this so a field session can see what they rejected without a crash.
    const char* path = std::getenv("MIKUDANCESTUDIO_TRACE_FILE");
    if (path == nullptr || path[0] == '\0')
        return;
    FILE* stream = std::fopen(path, "a");
    if (stream == nullptr)
        return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(stream, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute,
                 now.wSecond, now.wMilliseconds);
    std::vfprintf(stream, format, args);
    std::fputc('\n', stream);
    std::fflush(stream);
    std::fclose(stream);
}

void Trace(const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    TraceV(format, args);
    va_end(args);
}

void Heartbeat(int frame) noexcept {
    ULONGLONG now = GetTickCount64();
    if (now - lastHeartbeat < 30000) return;
    lastHeartbeat = now;
    Write("ALIVE frame=%d", frame);
}

void NormalExit(int code) noexcept {
    SetPhase("process teardown");
    Write("NORMAL_EXIT code=%d", code);
}

void BeginShutdown() noexcept {
    Write("SHUTDOWN begin");
    SetPhase("process teardown");
    shuttingDown.store(true, std::memory_order_relaxed);
}

[[noreturn]] void Fatal(const char* reason) noexcept {
    Fail(reason != nullptr ? reason : "fatal error", 0xE0000004, _ReturnAddress());
}
}
