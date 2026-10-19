#pragma once

#include <array>
#include <mmsystem.h>

// Test-only observation of the existing device. Calls always reach the original
// methods, including MME's proxy; no Present or draw is suppressed.
namespace playback_benchmark {
struct DeviceObserver {
    using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using DrawFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using IndexedFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using UpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using IndexedUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
    inline static DeviceObserver* active = nullptr;
    void** table = nullptr;
    std::array<void*, 5> originals{};
    static constexpr unsigned slots[5] = {17, 81, 82, 83, 84};
    unsigned long long presents = 0, draws = 0, primitives = 0, failures = 0;
    unsigned installed = 0;
    static HRESULT WINAPI Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
        auto& o = *active;
        const auto hr = reinterpret_cast<PresentFn>(o.originals[0])(d, a, b, w, r);
        if (hr == D3D_OK) ++o.presents; else ++o.failures;
        return hr;
    }
    static HRESULT WINAPI Draw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT v, UINT n) {
        auto& o = *active; ++o.draws; o.primitives += n;
        return reinterpret_cast<DrawFn>(o.originals[1])(d, t, v, n);
    }
    static HRESULT WINAPI Indexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT v, UINT s, UINT n) {
        auto& o = *active; ++o.draws; o.primitives += n;
        return reinterpret_cast<IndexedFn>(o.originals[2])(d, t, b, m, v, s, n);
    }
    static HRESULT WINAPI Up(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT s) {
        auto& o = *active; ++o.draws; o.primitives += n;
        return reinterpret_cast<UpFn>(o.originals[3])(d, t, n, v, s);
    }
    static HRESULT WINAPI IndexedUp(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT m, UINT v, UINT n, const void* i, D3DFORMAT f, const void* p, UINT s) {
        auto& o = *active; ++o.draws; o.primitives += n;
        return reinterpret_cast<IndexedUpFn>(o.originals[4])(d, t, m, v, n, i, f, p, s);
    }
    bool Install(IDirect3DDevice9* device) {
        table = *reinterpret_cast<void***>(device);
        active = this;
        void* replacements[] = {reinterpret_cast<void*>(Present), reinterpret_cast<void*>(Draw),
            reinterpret_cast<void*>(Indexed), reinterpret_cast<void*>(Up), reinterpret_cast<void*>(IndexedUp)};
        for (unsigned i = 0; i < 5; ++i) {
            DWORD old = 0;
            if (!VirtualProtect(table + slots[i], sizeof(void*), PAGE_READWRITE, &old)) return false;
            originals[i] = table[slots[i]];
            table[slots[i]] = replacements[i];
            DWORD ignored = 0;
            VirtualProtect(table + slots[i], sizeof(void*), old, &ignored);
            ++installed;
        }
        return true;
    }
    ~DeviceObserver() {
        for (unsigned i = 0; i < installed; ++i) {
            DWORD old = 0, ignored = 0;
            if (VirtualProtect(table + slots[i], sizeof(void*), PAGE_READWRITE, &old)) {
                table[slots[i]] = originals[i];
                VirtualProtect(table + slots[i], sizeof(void*), old, &ignored);
            }
        }
        active = nullptr;
    }
};

template<class Prepare>
int Run(mikumikustudio::MMDApp* app, int seconds, const wchar_t* jsonPath, Prepare prepare) {
    using namespace mikumikustudio;
    using Clock = std::chrono::steady_clock;
    auto* device = app->Renderer()->device;
    DeviceObserver observer;
    if (!observer.Install(device)) return 50;
    app->FpsLimit() = 1000.f;
    app->PlaybackStartsAtCurrentFrame() = 0;
    app->MilliToSec() = 0.001f;
    app->TimeNowLow() = timeGetTime();
    app->TimeNowHigh() = 0;
    SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStartFrameEdit), L"0");
    SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStopFrameEdit), L"300000");
    CommandDispatch(GetDlgItem(app->MainWindow(), panel::kPlayButton), MAKEWPARAM(408, BN_CLICKED));
    if (!app->PlaybackActive()) return 51;
    DWORD previousTick = app->TimeNowLow();
    struct Frame { double endMs, durationMs; unsigned long long presents, draws, primitives; int sceneFrame; };
    std::vector<Frame> frames;
    const auto warmupStart = Clock::now();
    auto sampleStart = warmupStart;
    bool sampling = false;
    int sceneStart = 0;
    unsigned long long previousPresents = 0, previousDraws = 0, previousPrimitives = 0;
    while (true) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return 52;
            if (message.message == WM_PAINT) ValidateRect(message.hwnd, nullptr);
            else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (IsWindowVisible(app->MainWindow()) || IsIconic(app->MainWindow()) ||
            device->TestCooperativeLevel() != D3D_OK || !app->PlaybackActive()) return 53;
        const auto now = Clock::now();
        if (!sampling && std::chrono::duration<double>(now - warmupStart).count() >= 5.0) {
            sampling = true;
            sampleStart = now;
            sceneStart = static_cast<int>(app->PlaybackCursorSeconds() * 30.f);
            previousPresents = observer.presents;
            previousDraws = observer.draws;
            previousPrimitives = observer.primitives;
        }
        if (sampling && std::chrono::duration<double>(now - sampleStart).count() >= seconds) break;
        prepare();
        // Same wall-clock input and cap compensation as WinMain. The normal
        // playback catch-up, physics, MME and Present all run in FrameDriver.
        DWORD tick = timeGetTime();
        const float delta = static_cast<float>(tick - previousTick) * 0.001f;
        app->DeltaTime() = delta;
        const float sleep = 1.f / app->FpsLimit() - delta;
        if (sleep > 0.f) {
            Sleep(static_cast<DWORD>(sleep * 1000.f));
            tick += static_cast<DWORD>(sleep / 0.001f);
            app->DeltaTime() += sleep;
        }
        app->TimeNowLow() = tick;
        app->TimeNowHigh() = 0;
        previousTick = tick;
        app->state.messageSeen = 10;
        FrameDriver(app);
        const auto end = Clock::now();
        if (sampling) {
            frames.push_back({std::chrono::duration<double, std::milli>(end - sampleStart).count(),
                std::chrono::duration<double, std::milli>(end - now).count(),
                observer.presents - previousPresents, observer.draws - previousDraws,
                observer.primitives - previousPrimitives, static_cast<int>(app->PlaybackCursorSeconds() * 30.f)});
            previousPresents = observer.presents;
            previousDraws = observer.draws;
            previousPrimitives = observer.primitives;
        }
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - sampleStart).count();
    bool valid = !frames.empty() && observer.failures == 0 && static_cast<int>(app->PlaybackCursorSeconds() * 30.f) > sceneStart;
    unsigned long long presents = 0, draws = 0, primitives = 0;
    for (const auto& f : frames) {
        valid = valid && f.presents == 1 && f.draws > 0;
        presents += f.presents; draws += f.draws; primitives += f.primitives;
    }
    std::wstring csvPath = std::wstring(jsonPath) + L".csv";
    FILE* csv = nullptr;
    if (_wfopen_s(&csv, csvPath.c_str(), L"wb") || !csv) return 54;
    std::fprintf(csv, "end_ms,frame_ms,presents,draws,primitives,scene_frame\n");
    for (const auto& f : frames) std::fprintf(csv, "%.6f,%.6f,%llu,%llu,%llu,%d\n",
        f.endMs, f.durationMs, f.presents, f.draws, f.primitives, f.sceneFrame);
    if (std::fclose(csv)) return 54;
    FILE* json = nullptr;
    if (_wfopen_s(&json, jsonPath, L"wb") || !json) return 54;
    std::fprintf(json,
        "{\"metric\":\"background_playback_present_fps\",\"valid\":%s,"
        "\"seconds\":%.6f,\"frames\":%zu,\"presents\":%llu,\"fps\":%.6f,"
        "\"draws\":%llu,\"primitives\":%llu,\"present_failures\":%llu,"
        "\"scene_start_frame\":%d,\"scene_end_frame\":%d,\"warmup_seconds\":5,"
        "\"hidden\":true,\"minimized\":false,\"fps_limit\":1000,\"mme\":%s}",
        valid ? "true" : "false", elapsed, frames.size(), presents, presents / elapsed,
        draws, primitives, observer.failures, sceneStart, static_cast<int>(app->PlaybackCursorSeconds() * 30.f),
        GetModuleHandleW(L"MMEffect.dll") ? "true" : "false");
    if (std::fclose(json)) return 54;
    std::printf("background_playback_present_fps=%.3f valid=%u frames=%zu\n", presents / elapsed, valid, frames.size());
    app->PlaybackActive() = 0;
    StopPlayback(app);
    return valid ? 0 : 55;
}
} // namespace playback_benchmark
