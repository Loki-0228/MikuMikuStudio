// Hidden integration: startup, menu, file drop, PMM audio and device retry.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_message.hpp"
#include <MinHook.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <vector>

namespace mikumikustudio {
void CmdOpenWave(MMDApp*);
void LoadSceneFile();
void WaveRestartAt(void*, double);
}
using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
unsigned creates = 0, errors = 0;
bool failDevice = false;
std::uintptr_t __cdecl FailFeedThread(void (__cdecl*)(void*), unsigned, void*) {
    return static_cast<std::uintptr_t>(-1);
}
struct CompletionRace {
    WaveAudioContext audio{};
    HANDLE ready = nullptr;
};
unsigned __stdcall CompleteFeed(void* value) {
    auto* race = static_cast<CompletionRace*>(value);
    WaitForSingleObject(race->ready, INFINITE);
    SetWaveStopState(&race->audio, 2);
    return 0;
}
const wchar_t* menuPath = nullptr;
decltype(&DirectSoundCreate) realCreate = nullptr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
    std::printf("PASS %s\n", what);
}
HRESULT WINAPI CountCreate(LPCGUID id, LPDIRECTSOUND* device, LPUNKNOWN outer) {
    ++creates;
    if (failDevice) { *device = nullptr; return DSERR_NODRIVER; }
    return realCreate(id, device, outer);
}
BOOL WINAPI SelectAudio(LPOPENFILENAMEW dialog) {
    if (!menuPath) return FALSE;
    wcscpy_s(dialog->lpstrFile, dialog->nMaxFile, menuPath);
    return TRUE;
}
int CaptureError(MMDApp*, HWND, UiMessage message, UINT, const wchar_t*) {
    Check(message == UiMessage::DirectSoundError, "expected audio-device error");
    ++errors;
    return IDOK;
}
void Hook(void* target, void* replacement, void** original = nullptr) {
    Check(MH_CreateHook(target, replacement, original) == MH_OK, "install test hook");
    Check(MH_EnableHook(target) == MH_OK, "enable test hook");
}
void Drop(const fs::path& path) {
    const auto text = path.wstring();
    HGLOBAL memory = GlobalAlloc(GHND, sizeof(DROPFILES) + (text.size() + 2) * sizeof(wchar_t));
    Check(memory != nullptr, "allocate drop-file payload");
    auto* files = static_cast<DROPFILES*>(GlobalLock(memory));
    files->pFiles = sizeof(DROPFILES);
    files->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(files) + sizeof(DROPFILES),
                text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    HandleDropFiles(static_cast<HDROP>(memory));
}
void Empty(MMDApp* app) {
    const auto* audio = app->Audio();
    Check(!app->WaveEnabled() && !app->DirectSoundAvailable() && !app->WavePath()[0],
          "empty project has no audio flags or path");
    Check(!audio->directSound && !audio->streamingBuffer && !audio->feedThread &&
          !audio->fileHandle && !audio->path[0] && !audio->waveformColumns,
          "empty project owns no sound resources");
}
void Loaded(MMDApp* app) {
    const auto* audio = app->Audio();
    Check(app->WaveEnabled() && app->DirectSoundAvailable() &&
          audio->directSound && audio->streamingBuffer && audio->waveformColumns > 0,
          "import has audio device, buffer and waveform");
    Check(audio->timelineDC == app->TimelineDC(), "waveform remains bound to timeline");
}
void Save(MMDApp* app, const fs::path& path) {
    wcscpy_s(app->EnvFileName(), 256, path.c_str());
    SaveSceneFile(app);
    Check(fs::exists(path), "save isolated PMM fixture");
}
void Open(MMDApp* app, const fs::path& path) {
    wcscpy_s(app->EnvFileName(), 256, path.c_str());
    LoadSceneFile();
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::wcscmp(argv[2], L"--stop-race") == 0) {
        CompletionRace race;
        race.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(race.ready != nullptr, "create completion-race event");
        for (int i = 0; i < 256; ++i) {
            race.audio = {};
            ResetEvent(race.ready);
            HANDLE worker = reinterpret_cast<HANDLE>(
                _beginthreadex(nullptr, 0, CompleteFeed, &race, 0, nullptr));
            if (!worker) return 2;
            race.audio.feedThread = reinterpret_cast<std::uintptr_t>(worker);
            SetEvent(race.ready);
            CloseDataFile(&race.audio);
            if (WaitForSingleObject(worker, 1000) != WAIT_OBJECT_0) return 3;
            CloseHandle(worker);
            if (race.audio.feedThread || WaveStopState(&race.audio) != 2) return 4;
        }
        CloseHandle(race.ready);
        Check(true, "256 concurrent feed completion and cleanup cycles");
        SetWaveStopState(&race.audio, 2);
        RequestWaveStop(&race.audio);
        Check(WaveStopState(&race.audio) == 2, "late stop request preserves published completion");
        return 0;
    }
    const fs::path directory = fs::absolute(argv[1]);
    fs::create_directories(directory);
    const auto wave = directory / L"silent.wav";
    // One second of silent PCM16 stereo exercises streaming without audible output.
    std::ofstream out(wave, std::ios::binary);
    const auto u32 = [&](std::uint32_t value) { out.write(reinterpret_cast<char*>(&value), 4); };
    const auto u16 = [&](std::uint16_t value) { out.write(reinterpret_cast<char*>(&value), 2); };
    out.write("RIFF", 4); u32(176400 + 36); out.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(44100); u32(176400); u16(4); u16(16);
    out.write("data", 4); u32(176400);
    const std::vector<char> silence(176400); out.write(silence.data(), silence.size()); out.close();
    Check(MH_Initialize() == MH_OK, "initialize interception");
    Hook(reinterpret_cast<void*>(&DirectSoundCreate), reinterpret_cast<void*>(&CountCreate),
         reinterpret_cast<void**>(&realCreate));
    Hook(reinterpret_cast<void*>(&GetOpenFileNameW), reinterpret_cast<void*>(&SelectAudio));
    Hook(reinterpret_cast<void*>(&ShowUiMessage), reinterpret_cast<void*>(&CaptureError));
    auto* app = new MMDApp();
    g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "hidden application startup");
    Empty(app);
    Check(creates == 0, "startup makes no DirectSound calls");
    WaveRestartAt(app->Audio(), 0.0);
    CmdOpenWave(app);
    Empty(app);
    Check(creates == 0, "empty seek and cancelled import make no DirectSound calls");
    const auto waveText = wave.wstring();
    menuPath = waveText.c_str();
    CmdOpenWave(app);
    Loaded(app);
    Check(creates == 1, "menu import creates device once");
    if (argc == 3 && std::wcscmp(argv[2], L"--feed-failure") == 0) {
        Hook(reinterpret_cast<void*>(&_beginthread), reinterpret_cast<void*>(&FailFeedThread));
        WaveSeekAndFeed(app->Audio(), 0.0);
        const bool noWorker = app->Audio()->feedThread == 0;
        const bool released = WaveStopState(app->Audio()) == 2 && !app->Audio()->streamingBuffer;
        std::puts("cleanup after injected feed-thread creation failure");
        const auto started = GetTickCount64();
        CloseDataFile(app->Audio());
        Check(GetTickCount64() - started < 1000, "cleanup after failed feed start cannot hang");
        Check(noWorker, "failed feed start does not leave a phantom worker");
        Check(released, "failed feed start releases its buffer and publishes completion");
        Check(MH_DisableHook(reinterpret_cast<void*>(&_beginthread)) == MH_OK,
              "restore feed thread creation");
        WaveRestartAt(app->Audio(), 0.0);
        Sleep(40);
        Check(app->Audio()->feedThread != 0, "audio seek recovers after failed feed start");
        ResetAppState(app);
        Empty(app);
        ShutdownCleanup(app);
        delete app;
        g_Block = nullptr;
        MH_Uninitialize();
        return 0;
    }
    WaveSeekAndFeed(app->Audio(), 0.0);
    Sleep(40);
    Check(app->Audio()->feedThread != 0, "silent playback starts feed thread");
    ResetAppState(app);
    Empty(app);
    Check(creates == 1, "new project releases playing audio without initialization");
    Drop(wave);
    Loaded(app);
    Check(creates == 2, "dragged audio initializes device");
    const auto project = directory / L"with-audio.pmm";
    Save(app, project);
    ResetAppState(app);
    Open(app, project);
    Loaded(app);
    Check(creates == 3, "PMM with audio initializes device");
    ResetAppState(app);
    const auto emptyProject = directory / L"empty.pmm";
    Save(app, emptyProject);
    Open(app, emptyProject);
    Empty(app);
    Check(creates == 3, "empty PMM never initializes audio");
    failDevice = true;
    Drop(wave);
    Check(!app->WaveEnabled() && !app->Audio()->directSound &&
          !app->Audio()->fileHandle && errors == 1, "failed device creation leaves no playback or file handle");
    failDevice = false;
    Drop(wave);
    Loaded(app);
    Check(creates == 5, "import retries successfully after device failure");
    Drop(wave);
    Loaded(app);
    Check(creates == 5, "repeated imports reuse initialized device");
    Check(!IsWindowVisible(app->MainWindow()), "all checks kept main window hidden");
    ResetAppState(app);
    ShutdownCleanup(app);
    delete app;
    g_Block = nullptr;
    MH_Uninitialize();
    std::puts("PASS lazy audio integration");
    return 0;
}
