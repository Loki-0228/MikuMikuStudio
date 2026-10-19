#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/recording_state.hpp"
#include "../src/app/recording_ack_wait.hpp"
#include <cstdio>
#include <limits>
#include <initializer_list>
#include <utility>
using namespace mikumikustudio;
struct FakePush final : IPushSource {
    unsigned calls = 0, readyAfter = 2;
    bool ended = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetBitmapInfo(const void*, int, float) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamingState(void* p) override {
        if (ended) return E_FAIL;
        *static_cast<unsigned char*>(p) = ++calls >= readyAfter; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartStreaming(DWORD_PTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginStreaming() override { ended = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRate(float*) override { return S_OK; }
};
int main() {
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool value, const char* message) { ++checks; if (!value) { ++failures; std::printf("FAIL %s\n", message); } };
    using namespace recording_state;
    for (float invalid : {0.f, -.5f, 241.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        check(!ValidFps(invalid) && FrameDuration(invalid) == 0, "invalid rate rejected");
    check(FrameDuration(.5f) == 20000000, "sub-one FPS remains valid");
    check(FrameDuration(29.97f) == 333667, "fractional FPS is retained");
    for (const auto pair : {std::pair<float, unsigned>{15, 3}, {24, 5}, {30, 6}, {60, 12}, {120, 24}})
        check(OutputFrameCount(0, 5, pair.first) == pair.second, "completion count follows output FPS");
    check(OutputFrameCount(100, 105, 60) == 12, "nonzero range start");
    int app;
    {
        Guard outer(&app, Operation::Finish), nested(&app, Operation::Finish), graph(&app, Operation::GraphTeardown);
        check(outer && !nested && graph && Stopping(&app), "same session stops once, graph scope remains independent");
    }
    check(!Stopping(&app), "stop scope is released");
    FakePush push; unsigned char flag = 0; unsigned pumps = 0;
    check(WaitForAck(&push, &flag, [] { return true; }, [&] { ++pumps; return true; }, 100) == AckResult::Ready && flag == 1 && pumps == 1, "pending frame pumps and acknowledges");
    push.ended = true;
    check(WaitForAck(&push, &flag, [] { return true; }, [] { return true; }) == AckResult::Ended, "delivery failure terminates wait");
    push.ended = false; push.calls = 0; push.readyAfter = ~0u; bool current = true;
    check(WaitForAck(&push, &flag, [&] { return current; }, [&] { current = false; return true; }) == AckResult::Cancelled, "dispatch cancellation invalidates session before reuse");
    check(WaitForAck(&push, &flag, [] { return true; }, [] { return false; }) == AckResult::Cancelled, "quit cancels wait");
    check(WaitForAck(&push, &flag, [] { return true; }, [] { return true; }, 5) == AckResult::TimedOut, "nonresponding encoder has bounded wait");
    std::printf("recording state: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
