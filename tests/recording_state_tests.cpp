#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/recording_state.hpp"
#include "../src/app/recording_ack_wait.hpp"
#include <cstdio>
#include <limits>
#include <initializer_list>
#include <utility>
#include <array>
using namespace mikumikustudio;
struct FakePush final : IPushSource {
    unsigned calls = 0, readyAfter = 2;
    bool ended = false;
    HRESULT failure = E_FAIL;
    bool* invalidateOnRead = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetBitmapInfo(const void*, int, float) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamingState(void* p) override {
        if (invalidateOnRead) *invalidateOnRead = false;
        if (ended) return failure;
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
    for (float invalid : {0.f, -.5f, .0001f, 241.f, std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        check(!ValidFps(invalid) && FrameDuration(invalid) == 0, "invalid rate rejected");
    check(FrameDuration(.5f) == 20000000, "sub-one FPS remains valid");
    check(FrameDuration(29.97f) == 333667, "fractional FPS is retained");
    check(ValidFps(.001f) && ValidFps(240.f), "documented rate endpoints remain usable");
    for (const auto pair : {std::pair<float, unsigned>{15, 3}, {24, 5}, {30, 6}, {60, 12}, {120, 24}})
        check(OutputFrameCount(0, 5, pair.first) == pair.second, "completion count follows output FPS");
    check(OutputFrameCount(100, 105, 60) == 12, "nonzero range start");
    check(OutputFrameCount(5, 4, 30) == 0 && OutputFrameCount(0, 5, 0) == 0,
          "invalid ranges and frame rates cannot report successful output");
    check(OutputFrameCount(0, (std::numeric_limits<std::int32_t>::max)(), 240) == 17179869184ull,
          "completion count promotes the inclusive range before subtracting or multiplying");
    for (const auto first : {0, 100}) for (const auto lastOffset : {0, 5, 20})
        for (const auto fps : { .5f, 15.f, 24.f, 29.97f, 30.f, 60.f, 120.f }) {
            std::uint64_t delivered = 0;
            // Independent oracle: the production catch-up schedules a sample
            // while floor(N*30/fps + first) remains inside the inclusive range.
            while (static_cast<std::int64_t>(double(delivered) * 30.0 / fps + first) < first + lastOffset + 1)
                ++delivered;
            check(OutputFrameCount(first, first + lastOffset, fps) == delivered,
                  "completion count agrees with output-time scheduling, including fractional rates");
        }
    int app;
    {
        Guard outer(&app, Operation::Finish), nested(&app, Operation::Finish), graph(&app, Operation::GraphTeardown);
        check(outer && !nested && graph && Stopping(&app), "same session stops once, graph scope remains independent");
    }
    check(!Stopping(&app), "stop scope is released");
    int other;
    {
        Guard first(&app, Operation::Finish), second(&other, Operation::Finish);
        check(first && second && Stopping(&app) && Stopping(&other), "a different session can stop inside a modal dispatch");
        check(Guard::DeferClose(&app, Operation::Finish, 12) && first.DeferredClose() == 12 &&
              second.DeferredClose() == 0, "deferred close belongs to the matching outer session");
        check(Guard::DeferClose(&app, Operation::Finish, 12) && first.DeferredClose() == 12,
              "repeated close requests are coalesced during one stop");
    }
    check(!Guard::DeferClose(&app, Operation::Finish, 12), "inactive stops do not retain close requests");
    check(!Guard::DeferFinish(&app), "inactive graph builds cannot retain finish requests");
    {
        Guard first(&app, Operation::GraphBuild);
        check(first && !first.DeferredFinish() && Guard::Active(&app, Operation::GraphBuild),
              "graph build starts with no deferred cancellation");
        check(!Guard::DeferFinish(&other) && !first.DeferredFinish(),
              "finish for another application cannot cancel this graph build");
        {
            Guard unrelated(&other, Operation::Finish), sameAppOtherOperation(&app, Operation::GraphTeardown);
            check(Guard::DeferFinish(&app) && first.DeferredFinish() && !unrelated.DeferredFinish() &&
                  !sameAppOtherOperation.DeferredFinish(), "defer searches the matching graph build across other guard operations");
            check(Guard::DeferFinish(&app) && Guard::DeferFinish(&app) && first.DeferredFinish(),
                  "repeated cancellation requests coalesce into the same graph-build flag");
        }
        {
            Guard second(&other, Operation::GraphBuild), rejectedNested(&app, Operation::GraphBuild);
            check(second && !rejectedNested && !second.DeferredFinish() && !rejectedNested.DeferredFinish(),
                  "another app can build independently while duplicate same-app build is rejected");
            check(Guard::DeferFinish(&other) && second.DeferredFinish() && first.DeferredFinish(),
                  "each application retains its own deferred cancellation flag");
        }
        check(!Guard::DeferFinish(&other) && first.DeferredFinish(),
              "exiting a nested foreign build leaves only the outer cancellation active");
    }
    check(!Guard::DeferFinish(&app) && !Guard::DeferFinish(&other),
          "finished build scopes cannot retain cancellation flags");
    {
        Guard first(&app, Operation::GraphBuild), second(&other, Operation::GraphBuild);
        check(Guard::DeferFinish(&app) && first.DeferredFinish() && !second.DeferredFinish(),
              "cancelling the outer app does not mark the innermost foreign build");
        check(Guard::DeferFinish(&other) && second.DeferredFinish(),
              "the independent inner app accepts its own cancellation");
    }
    {
        Guard next(&app, Operation::GraphBuild);
        check(next && !next.DeferredFinish(), "a later build does not inherit a previous cancellation");
    }
    {
        Guard finishing(&app, Operation::Finish), teardown(&app, Operation::GraphTeardown);
        check(!Guard::DeferFinish(&app) && !finishing.DeferredFinish() && !teardown.DeferredFinish(),
              "finish and teardown guards alone are not graph-build cancellation targets");
    }
    FakePush push; unsigned char flag = 0; unsigned pumps = 0;
    check(WaitForAck(&push, &flag, [] { return true; }, [&] { ++pumps; return true; }, 100) == AckResult::Ready && flag == 1 && pumps == 1, "pending frame pumps and acknowledges");
    push.ended = true;
    check(WaitForAck(&push, &flag, [] { return true; }, [] { return true; }) == AckResult::Ended, "delivery failure terminates wait");
    push.failure = E_ABORT;
    check(WaitForAck(&push, &flag, [] { return true; }, [] { return true; }) == AckResult::Ended,
          "every failed status terminates wait rather than only E_FAIL");
    push.ended = false; push.calls = 0; push.readyAfter = ~0u; bool current = true;
    check(WaitForAck(&push, &flag, [&] { return current; }, [&] { current = false; return true; }) == AckResult::Cancelled, "dispatch cancellation invalidates session before reuse");
    check(WaitForAck(&push, &flag, [] { return true; }, [] { return false; }) == AckResult::Cancelled, "quit cancels wait");
    check(WaitForAck(&push, &flag, [] { return true; }, [] { return true; }, 5) == AckResult::TimedOut, "nonresponding encoder has bounded wait");
    push.calls = 0;
    check(WaitForAck(&push, &flag, [] { return false; }, [] { return true; }) == AckResult::Cancelled && push.calls == 0,
          "an already replaced session does not call a stale interface");
    current = true; push.calls = 0; push.readyAfter = 1; push.invalidateOnRead = &current; flag = 71;
    check(WaitForAck(&push, &flag, [&] { return current; }, [] { return true; }) == AckResult::Cancelled && flag == 71,
          "a COM call that replaces the session cannot write through its old completion pointer");
    check(WaitForAck(nullptr, &flag, [] { return true; }, [] { return true; }) == AckResult::Ended,
          "missing sources end an explicit wait without a null dereference");
    std::printf("recording state: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
