#pragma once
#include "mikudancestudio/update_coordinator.hpp"
#include <MinHook.h>
#include <cstdlib>
#include <cstdio>
// Opt-in adapter for existing end-to-end export fixtures. Windows stay hidden;
// only the scheduler's availability query is controlled. No frame callback is
// replaced and output sampling still uses the production recording path.
namespace coordinator_fixture {
inline bool Enabled() { return GetEnvironmentVariableW(L"MMS_TEST_COORDINATOR",nullptr,0)!=0; }
inline decltype(&IsWindowVisible) originalVisible=nullptr;
inline decltype(&mikudancestudio::FrameDriver) originalFrame=nullptr;
inline uint64_t frames=0, polls=0;
inline void ObserveFrame(mikudancestudio::MMDApp* app) {
    ++frames;
    const float delta=app->DeltaTime();
    const bool recording=app->RecordingWindow()!=nullptr;
    originalFrame(app);
    if(recording && app->DeltaTime()!=delta) {
        std::fputs("FAIL export did not restore the coordinator delta\n",stderr);std::abort();
    }
}
inline BOOL WINAPI Available(HWND window) {
    auto* app=mikudancestudio::g_Block;
    return app && (window==app->MainWindow() || window==app->FloatingWindow()) ? TRUE : originalVisible(window);
}
inline void Prepare(mikudancestudio::MMDApp* app) {
    using namespace mikudancestudio;
    if(!originalVisible) {
        if(MH_CreateHook(reinterpret_cast<void*>(&IsWindowVisible),reinterpret_cast<void*>(&Available),
            reinterpret_cast<void**>(&originalVisible))!=MH_OK || MH_EnableHook(reinterpret_cast<void*>(&IsWindowVisible))!=MH_OK) std::abort();
        if(MH_CreateHook(reinterpret_cast<void*>(&mikudancestudio::FrameDriver),reinterpret_cast<void*>(&ObserveFrame),
            reinterpret_cast<void**>(&originalFrame))!=MH_OK || MH_EnableHook(reinterpret_cast<void*>(&mikudancestudio::FrameDriver))!=MH_OK) std::abort();
        if(!updates::Initialize(app)) std::abort();
        app->FpsLimit()=1000;
    }
}
inline void Poll(mikudancestudio::MMDApp* app) {
    Prepare(app);
    Sleep(3);++polls;mikudancestudio::updates::SafePoint(app);
}
inline void Drive(mikudancestudio::MMDApp* app) {
    using namespace mikudancestudio;
    if(!Enabled()) { FrameDriver(app);return; }
    Prepare(app);updates::RequestViewport(app);
    // A wake is not a frame: the real coordinator may still be before its
    // deadline. Keep the existing per-frame budgets and count actual driver
    // entries, with a wall-clock bound that also catches a stalled scheduler.
    const auto before=frames, deadline=GetTickCount64()+5000;
    do {
        Sleep(std::max<DWORD>(1,updates::WaitMilliseconds(app)));
        ++polls;updates::SafePoint(app);
        if(GetTickCount64()>=deadline && frames==before) {
            std::fprintf(stderr,"FAIL coordinator frame timeout polls=%llu frames=%llu\n",
                static_cast<unsigned long long>(polls),static_cast<unsigned long long>(frames));std::abort();
        }
    } while(frames==before);
}
}
