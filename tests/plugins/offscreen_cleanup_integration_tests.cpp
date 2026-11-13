// Compiles the real integration implementation into this test translation
// unit so the private cleanup bridge is exercised, not a reimplementation.
// Link against the normal full-profile core and do not also compile
// integration.cpp as a separate source for this target.
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {
bool failTimer = false, failPost = false;
UINT_PTR WINAPI TestSetTimer(HWND hwnd, UINT_PTR id, UINT ms, TIMERPROC callback) {
    return failTimer ? 0 : ::SetTimer(hwnd, id, ms, callback);
}
BOOL WINAPI TestPostMessageW(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    return failPost ? FALSE : ::PostMessageW(hwnd, message, wp, lp);
}
}
#define SetTimer TestSetTimer
#define PostMessageW TestPostMessageW
#include "../../src/plugins/integration.cpp"
#undef SetTimer
#undef PostMessageW

namespace mikumikustudio::plugins {
namespace {
unsigned checks = 0;
void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
std::uint64_t Thread(void*) noexcept { return GetCurrentThreadId(); }
struct Fixture {
    bool deleteDc = false;
    bool attemptReentry = false;
    bool reentryRejected = false;
    unsigned deletes = 0;
    int pixels = 0;
    OffscreenSurfaceOps Ops() noexcept {
        return {this,
            [](void*,std::uintptr_t) noexcept -> std::uintptr_t { return 11; },
            [](void* p,std::uintptr_t,int,int,void** bits) noexcept -> std::uintptr_t { *bits=&static_cast<Fixture*>(p)->pixels; return 22; },
            [](void*,std::uintptr_t,std::uintptr_t) noexcept { return true; },
            [](void*,std::uintptr_t) noexcept { return true; },
            {this,
                [](void*,std::uintptr_t,std::uintptr_t,std::uintptr_t* prior) noexcept { *prior=33; return true; },
                [](void*,std::uintptr_t,std::uintptr_t) noexcept { return true; },
                [](void* p,std::uintptr_t) noexcept {
                    auto& f=*static_cast<Fixture*>(p); ++f.deletes;
                    if (f.attemptReentry) {
                        const auto before=f.deletes; PumpOffscreenCleanup();
                        f.reentryRejected=offscreenCleanupBusy && f.deletes==before;
                    }
                    return f.deleteDc;
                },
                [](void*,std::uintptr_t) noexcept { return true; }} };
    }
};
OffscreenRuntimeHandle Make(OffscreenGdiRuntimeCore<16>& runtime, Fixture& fixture) {
    auto h=runtime.Reserve(fixture.Ops());
    Check(h.epoch != 0,"reserve surface");
    Check(runtime.Initialize(h,1,8,8)==OffscreenSurfaceInit::Ready,"initialize surface");
    return h;
}
void DeliverMessages(HWND window, DWORD milliseconds) {
    const auto deadline=GetTickCount64()+milliseconds;
    while (GetTickCount64()<deadline) {
        MSG message{};
        while (PeekMessageW(&message,window,0,0,PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
}
void Run() {
    // This dedicated process uses only a message-only window and fake resource
    // operations. It does not create, paint or activate a visible window.
    Check(!manager && !ui && !owner,"dedicated process has no live plugin host");
    OffscreenGdiRuntimeCore<16> runtime({nullptr,Thread});
    Check(runtime.Start(),"start test runtime"); offscreenRuntime=&runtime;
    offscreenRuntimeReady=true; closing=false;
    Check(EnsureOffscreenCleanupWindow(),"create message-only cleanup window");
    const HWND window=offscreenCleanupWindow;

    Fixture f; auto first=Make(runtime,f);
    Check(!runtime.Release(first),"failed close keeps resource");
    failTimer=true; failPost=true; WakeOffscreenCleanup();
    Check(offscreenCleanupPending && !offscreenWakePosted,"dual scheduling failure preserves pending work");
    f.attemptReentry=true; PumpOffscreenCleanup();
    Check(f.reentryRejected && offscreenCleanupPending,"nested pump is rejected and failure remains pending");
    f.deleteDc=true; PumpOffscreenCleanup();
    Check(!offscreenCleanupPending,"later explicit safe point completes cleanup");
    std::size_t occupied=99,pending=99; bool isClosing=true;
    Check(runtime.Counts(occupied,pending,isClosing) && !occupied && !pending,"resources released after retry");

    Fixture timer; auto second=Make(runtime,timer); Check(!runtime.Release(second),"timer case pending");
    failPost=true; failTimer=false; WakeOffscreenCleanup();
    Check(offscreenCleanupPending && !offscreenWakePosted,"failed post leaves timer fallback");
    timer.deleteDc=true; DeliverMessages(window,350);
    Check(!offscreenCleanupPending,"timer retries when post fails");

    Fixture posted; auto third=Make(runtime,posted); Check(!runtime.Release(third),"post case pending");
    failPost=false; failTimer=true; posted.deleteDc=true; WakeOffscreenCleanup();
    Check(offscreenWakePosted,"post succeeds when timer fails");
    DeliverMessages(window,100);
    Check(!offscreenCleanupPending && !offscreenWakePosted,"posted message completes cleanup");

    Fixture shutdown; auto fourth=Make(runtime,shutdown); (void)fourth;
    offscreenRuntimeReady=false; offscreenShutdownPending=true; failPost=true; failTimer=true;
    closing=true; WakeOffscreenCleanup(); PumpOffscreenCleanup();
    Check(offscreenShutdownPending && offscreenCleanupPending,"failed shutdown remains pending after host close");
    shutdown.deleteDc=true; PumpOffscreenCleanup();
    Check(!offscreenShutdownPending && !offscreenCleanupPending && !offscreenRuntimeReady,"shutdown releases without reopening a missing host");
    Check(runtime.Start(),"runtime can restart after completed shutdown");

    failPost=false; failTimer=false; offscreenRuntime=nullptr;
    Check(DestroyWindow(window)!=0,"destroy test message-only window");
    Check(!offscreenCleanupWindow,"window destruction clears its identity");
}
}
}
int main() {
    try { mikumikustudio::plugins::Run(); std::printf("offscreen cleanup integration PASS: %u checks\n",mikumikustudio::plugins::checks); return 0; }
    catch(const std::exception& error) { std::fprintf(stderr,"offscreen cleanup integration FAIL: %s\n",error.what()); return 1; }
}
