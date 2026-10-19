#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
// ===========================================================================
// VA 0x004C4460 - WinMain  (original: _WinMain@16)
// ===========================================================================
// Full 1:1 port.  Flow (verified against decompilation + disassembly):
//   1. operator new(0xA4530) -> ctor (0x42AE60) -> Block (0x54593C)
//      -> memset 0 -> InitDefaults (0x40A730)
//   2. command line -> ConvertAnsiToWide (0x407A70) into wchar_t[256]
//      @ this+0xA0900;  empty command line -> swprintf_s(buf, 0x100, L"%s%s")
//      with ZERO varargs (faithful to the original call site).
//   3. InitMainWindowAndD3D (0x47A5B0);  failure -> return 0.
//   4. FPS-capped PeekMessage loop; idle branch computes the frame delta
//      from timeGetTime() and calls the frame driver (0x46B090).
//   5. WM_QUIT -> ShutdownCleanup (0x462C40) -> free(Block), Block = 0.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <timeapi.h>   // timeGetTime (excluded by WIN32_LEAN_AND_MEAN)
#include <shellapi.h>
#include <new>

#include <cstdint>
#include <cwchar>
#include <cstdio>

#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/d3d12_native.hpp"

// Ask hybrid graphics drivers to prefer the high-performance adapter.
// On the tested Intel/NVIDIA laptop, D3D9 exposes only the integrated GPU
// without these hints and only the discrete GPU with them. Driver/OS policy
// still determines visibility; this is not arbitrary per-device switching.
// The explicit MME Exp* ordinals (@1..@37) remain unchanged.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nShowCmd) {
    using namespace mikumikustudio;

    runtime_log::Initialize();
    // The legacy UI uses fixed 96-DPI coordinates for both GDI drawing and
    // child controls. Keep Windows bitmap scaling active before any window
    // or screen metric is created/read. The manifest sets the process default;
    // the thread setting also handles a launcher/compatibility DPI override.
    // Do not restore the old context while this UI thread is running.
    using SetThreadDpiContext = HANDLE(WINAPI*)(HANDLE);
    const auto setThreadDpiContext = reinterpret_cast<SetThreadDpiContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if (setThreadDpiContext) {
        const HANDLE previous = setThreadDpiContext(
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-1))); // DPI_AWARENESS_CONTEXT_UNAWARE
        const DWORD error = previous ? ERROR_SUCCESS : GetLastError();
        runtime_log::Write("UI DPI: Windows scaling, thread context set=%d error=%lu",
                           previous != nullptr, error);
    }
    // Global processor policy (per Windows user): read and freeze once here,
    // before any device exists.  The legacy Data\skinning.ini is not read; the
    // frozen Vertices choice becomes the skinning start preference inside
    // Initialize.  Later UI edits only wait for the next launch.
    std::wstring processorError;
    if (!processor::Initialize(processorError)) {
        runtime_log::Write("PROCESSOR settings init failed: %ls",
                           processorError.c_str());
    } else if (!processorError.empty()) {
        runtime_log::Write("PROCESSOR settings warning: %ls",
                           processorError.c_str());
    }
    runtime_log::SetPhase("construct application");

    (void)hPrevInstance;

    // operator new(0xA4530) with the original's null-check (VC9 new
    // semantics), then ctor 0x42AE60 and the global Block assignment.
    MMDApp* app = new (std::nothrow) MMDApp();
    g_Block = app;
    if (app == nullptr) {
        // The original returned 0 here and vanished without a word; the
        // startup report keeps the failure visible (dialog, see runtime_log).
        runtime_log::Fatal("application allocation failed (内存不足 / out of memory)");
    }
    app->state = MMDAppState{};  // original: memset(p, 0, 0xA4530)
    app->InitDefaults();                                          // 0x40A730

    // WinMain's ANSI command line has already lost characters outside ACP.
    // Parse the original UTF-16 command line, including quoted paths/spaces.
    (void)lpCmdLine;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t* scenePath = nullptr;
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--d3d12-preview") == 0) EnableD3D12Native(true);
        else if (!scenePath) scenePath = argv[i];
    }
    if (scenePath) {
        wcscpy_s(app->EnvFileName(), 0x100, scenePath);
    } else {
        // Original pushes only Format/BufferCount/Buffer - no varargs
        // (verified in disassembly at 0x4C44CA..0x4C44DA).  VC9 read two
        // garbage stack "pointers" and produced a junk path that the load
        // step then failed to open.  Deterministic equivalent: two empty
        // strings, so the buffer ends up empty and the load fails the same
        // way without relying on undefined behaviour of the modern CRT.
        swprintf_s(app->EnvFileName(), 0x100, g_SourceFormat, L"", L"");
    }
    if (argv) LocalFree(argv);

    runtime_log::Write("INIT window and Direct3D begin");
    runtime_log::SetPhase("initialize window and Direct3D");
    if (!InitMainWindowAndD3D(g_Block, hInstance, nShowCmd)) {     // 0x47A5B0
        // Same deviation as above: the original left with code 0 and no
        // message, which is exactly the silent launch failure the startup
        // report now surfaces.
        runtime_log::Fatal(
            "window / Direct3D initialization failed (窗口或 Direct3D 初始化失败)");
    }
    runtime_log::Write("INIT complete; entering message loop");

    autosave::Initialize(app);
    MSG msg{};
    std::uint32_t timeHigh = 0;         // v7/ebp - high 32 bits of last tick
    DWORD timeLow = timeGetTime();      // Time/ebx
    app->MilliToSec() = 0.001f;         // [Block+0xA0B70] = flt_5318D0

    PeekMessageW(&msg, nullptr, 0, 0, 0);  // PM_NOREMOVE prime, original arg set
    // DIAG(fps): once per second append loop/pump/sleep statistics to the
    // file named by MIKUMIKUSTUDIO_PUMP_STATS.  Purely observational -
    // the loop below is untouched when the variable is unset.
    char pumpStatsPath[MAX_PATH]{};
    std::FILE* pumpStats = nullptr;
    if (GetEnvironmentVariableA("MIKUMIKUSTUDIO_PUMP_STATS",
                                pumpStatsPath, MAX_PATH) > 0) {
        pumpStats = std::fopen(pumpStatsPath, "a");
    }
    DWORD statsT0 = timeGetTime();
    std::uint64_t statsLoops = 0, statsMsgs = 0, statsPumps = 0;
    double statsSleepMs = 0.0, statsPumpMs = 0.0;
    while (msg.message != WM_QUIT) {       // 18
        runtime_log::SetPhase("message loop");
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            runtime_log::SetPhase("dispatch window message");
            if (msg.message == WM_CLOSE || msg.message == WM_QUIT)
                runtime_log::Write("EXIT_MESSAGE message=0x%04X code=%llu", msg.message,
                    static_cast<unsigned long long>(msg.wParam));
            if (!bake::UiLocked() && msg.message==WM_KEYDOWN && !(msg.lParam&(1LL<<30)) &&
                (msg.hwnd==app->MainWindow()||IsChild(app->MainWindow(),msg.hwnd)) &&
                plugin_menu::HandleShortcut(app,unsigned(msg.wParam))) continue;
            // Forward system shortcuts from focused child/floating controls.
            if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_RETURN &&
                (msg.lParam & (LPARAM(1) << 29)) != 0 &&
                (msg.hwnd == app->MainWindow() || IsChild(app->MainWindow(), msg.hwnd) ||
                 (app->FloatingWindow() && (msg.hwnd == app->FloatingWindow() || IsChild(app->FloatingWindow(), msg.hwnd))))) {
                SendMessageW(app->MainWindow(), msg.message, msg.wParam, msg.lParam);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (pumpStats != nullptr) ++statsMsgs;
        } else {
            autosave::Tick(app, GetTickCount64());
            std::uint32_t nowLow = timeGetTime();           // v9
            std::uint32_t nowHigh = 0;                       // v10
            // v13 = (now - last) * 0.001.  x64 0x7FF7CB4FB4B0..0x7FF7CB4FB4B8:
            // the subtraction runs against the FULL 64-bit last tick
            // (sub rax, rdi), then one cvtsi2ss and a single-precision
            // mulss by the 0.001f slot (xmm7, same bits as MilliToSec).
            // The old port multiplied in double.
            const std::int64_t lastTick =
                (static_cast<std::int64_t>(timeHigh) << 32) | timeLow;
            float delta = static_cast<float>(
                static_cast<std::int64_t>(nowLow) - lastTick) * 0.001f;
            app->DeltaTime() = delta;                        // [Block+0xA077C]

            // v14 = 1/fpsLimit - delta (divss + subss, both float);
            // Sleep when cap enabled and positive.
            float sleepSec = 1.0f / app->FpsLimit() - delta;
            if (app->RecordingWindow() == nullptr && sleepSec > 0.0f) {
                DWORD pre = timeGetTime();
                Sleep(static_cast<DWORD>(sleepSec * 1000.0f));
                // x64 0x7FF7CB4FB4FD: divss by the same 0.001f slot, then
                // cvttss2si r64.  Float division, not double: the last-ulp
                // difference is magnified by the truncation below.
                const std::uint64_t addMs = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(sleepSec / 0.001f));
                // The original keeps the tick in one 64-bit register and adds the
                // compensation with a single instruction (add rbx, rax at
                // 0x7FF7CB4FB516, on the value stored at Block+0xA1BB8), so a
                // carry out of the low word propagates to the high word. Writing
                // the two halves separately lost that carry, so the tick no longer
                // matched the value the original stores.
                // Defect AB-04.
                const std::uint64_t tick =
                    (static_cast<std::uint64_t>(nowHigh) << 32) + nowLow +
                    addMs;
                nowLow = static_cast<std::uint32_t>(tick);
                nowHigh = static_cast<std::uint32_t>(tick >> 32);
                app->DeltaTime() = sleepSec + app->DeltaTime();
                if (pumpStats != nullptr) statsSleepMs += timeGetTime() - pre;
            }
            app->TimeNowLow() = nowLow;                      // [Block+0xA0B68]
            app->TimeNowHigh() = nowHigh;                    // [Block+0xA0B6C]
            timeLow = nowLow;
            timeHigh = nowHigh;

            DWORD pumpT0 = pumpStats != nullptr ? timeGetTime() : 0;
            runtime_log::SetPhase("FrameDriver");
            FrameDriver(g_Block);                            // 0x46B090
            runtime_log::SetPhase("message loop");
            runtime_log::Heartbeat(app->CurrentFrame());
            if (pumpStats != nullptr) {
                statsPumpMs += timeGetTime() - pumpT0;
                ++statsPumps;
                ++statsLoops;
                DWORD elapsed = timeGetTime() - statsT0;
                if (elapsed >= 1000) {
                    std::fprintf(pumpStats,
                        "loops=%llu msgs=%llu pump=%llu pumpMs=%.0f "
                        "sleepMs=%.0f fpsLimit=%.2f\n",
                        (unsigned long long)statsLoops,
                        (unsigned long long)statsMsgs,
                        (unsigned long long)statsPumps,
                        statsPumpMs, statsSleepMs, app->FpsLimit());
                    std::fflush(pumpStats);
                    statsT0 += elapsed;
                    statsLoops = statsMsgs = statsPumps = 0;
                    statsSleepMs = statsPumpMs = 0.0;
                }
            }
        }
    }
    if (pumpStats != nullptr) std::fclose(pumpStats);

    if (g_Block != nullptr) {
        // The message loop is over: from here on a fault is a teardown fault
        // and is recorded without the modal report (the window is gone and
        // the settings are already written).
        runtime_log::BeginShutdown();
        runtime_log::SetPhase("ShutdownCleanup");
        MMDApp* victim = g_Block;
        ShutdownCleanup(g_Block);                            // 0x462C40
        runtime_log::Write("SHUTDOWN cleanup complete");
        runtime_log::SetPhase("delete application");
        delete victim;
        g_Block = nullptr;
    }
    runtime_log::NormalExit(static_cast<int>(msg.wParam));
    return static_cast<int>(msg.wParam);
}
