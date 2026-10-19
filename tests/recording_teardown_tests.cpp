// Production graph teardown with real message dispatch and isolated COM peers.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dshow.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "mikumikustudio/dshow_recorder.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/recording_state.hpp"
#include "mikumikustudio/ui_metrics.hpp"
#include "../src/app/recording_ack_wait.hpp"

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}

template<class Interface> struct DispatchPeer : Interface {
    ULONG references = 1;
    unsigned additions = 0, releases = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { ++additions; return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; return --references; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
};
struct EventPeer final : DispatchPeer<IMediaEvent> {
    unsigned reads = 0, frees = 0;
    long completion = EC_COMPLETE;
    HRESULT STDMETHODCALLTYPE GetEventHandle(OAEVENT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEvent(long* code, LONG_PTR* first, LONG_PTR* second, long) override {
        ++reads;
        if (reads == 1) return E_ABORT;
        *code = completion; *first = 7; *second = 9;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForCompletion(long, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CancelDefaultHandling(long) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RestoreDefaultHandling(long) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreeEventParams(long code, LONG_PTR first, LONG_PTR second) override {
        Check(code == completion && first == 7 && second == 9, "only a successfully obtained event is freed");
        ++frees; return S_OK;
    }
};
struct ControlPeer final : DispatchPeer<IMediaControl> {
    OAFilterState state = State_Running;
    unsigned stops = 0;
    HRESULT STDMETHODCALLTYPE Run() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Pause() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Stop() override { ++stops; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetState(LONG, OAFilterState* value) override { *value = state; return S_OK; }
    HRESULT STDMETHODCALLTYPE RenderFile(BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddSourceFilter(BSTR, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_FilterCollection(IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_RegFilterCollection(IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE StopWhenReady() override { return E_NOTIMPL; }
};
struct PushPeer final : mikumikustudio::IPushSource {
    ULONG references = 1;
    unsigned begins = 0, releases = 0, stateReads = 0;
    HRESULT beginResult = S_OK;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; return --references; }
    HRESULT STDMETHODCALLTYPE SetBitmapInfo(const void*, int, float) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamingState(void*) override { ++stateReads; return E_FAIL; }
    HRESULT STDMETHODCALLTYPE StartStreaming(DWORD_PTR) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE BeginStreaming() override { ++begins; return beginResult; }
    HRESULT STDMETHODCALLTYPE GetRate(float*) override { return E_FAIL; }
};
struct Context {
    MMDApp* app = nullptr;
    DShowRecorder* recorder = nullptr;
    unsigned nested = 0, closes = 0;
};
constexpr UINT kReenter = WM_APP + 73;

void StoppedEntries(recording_state::Operation operation) {
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof app->state);
    DShowRecorder recorder{};
    PushPeer push; recorder.framePush = &push;
    app->Recorder() = &recorder;
    app->FrameStepPlayback() = app->PlaybackActive() = 1;
    app->state.messageSeen = 1;
    app->AviRecordFps() = 30;
    auto* previous = g_Block; g_Block = app.get();
    {
        const void* identity = operation == recording_state::Operation::Finish ?
            static_cast<const void*>(app.get()) : &recorder;
        recording_state::WindowStopGuard stopping(identity, operation);
        const auto entries = ui_metrics::counts[ui_metrics::FrameDriver].load();
        FrameDriver(app.get());
        Check(ui_metrics::counts[ui_metrics::FrameDriver].load() == entries,
              "paint-triggered FrameDriver refuses stopping before renderer, evaluation and frame counters");
        Check(!RecordingReadbackPass(app.get()) && !app->CaptureRenderTarget() && !app->CaptureSystemSurface() &&
              !app->CaptureReadbackPixels(), "direct readback refuses stopping before recovery and resource allocation");
        // With no models and range 0..0, the real catch-up immediately reaches
        // its private FrameStepBeyondEnd entry after incrementing N to one.
        PlaybackCatchup(app.get(), 0);
        Check(push.stateReads == 0 && app->FrameStepPlayback() == 1 && app->PlaybackActive() == 1,
              "the real past-end entry refuses stopping before polling or finishing the same recording");
    }
    Check(!recording_state::Stopping(app.get()) &&
          !recording_state::Guard::Active(&recorder, recording_state::Operation::GraphTeardown),
          "entry suppression ends with the stop scope");
    app->Recorder() = nullptr;
    g_Block = previous;
}
LRESULT CALLBACK Probe(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    if (context && message == kReenter) {
        ++context->nested;
        auto* event = context->recorder->mediaEvent;
        bool completed = true;
        Check(!TeardownDShowGraph(context->recorder, &completed) && !completed,
              "recursive production graph teardown is refused without consuming the completion event");
        Check(context->recorder->mediaEvent == event, "nested teardown leaves the outer graph owned");
        FinishAviRecord(context->app);
        Check(context->app->FrameStepPlayback() == 1 && context->app->PlaybackActive() == 1,
              "finish invoked during graph teardown cannot reset the active recording");
        RecWndProc(window, WM_CLOSE, 0, 0);
        RecWndProc(window, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(context->app->FrameStepPlayback() == 1, "record-window close and Escape do not start a second stop dialog");
        MainWndProc(window, WM_COMMAND, 200, 0);
        Check(context->app->state.dialogFlags[0] == 0, "queued main-window commands cannot run during the event drain");
        MainWndProc(window, WM_CLOSE, 0, 0);
        MainWndProc(window, WM_SYSCOMMAND, SC_CLOSE, 0);
        Check(context->closes == 0, "main-window close is deferred until the graph is released");
        return 0;
    }
    if (context && message == WM_CLOSE) {
        ++context->closes;
        Check(!recording_state::Guard::Active(context->recorder, recording_state::Operation::GraphTeardown),
              "deferred close dispatches after the stop guard has exited");
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void GraphCase(bool reenter, bool running, HRESULT beginResult, long completion, bool quit) {
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof app->state);
    DShowRecorder recorder{};
    EventPeer event;
    ControlPeer control; control.state = running ? State_Running : State_Stopped;
    PushPeer push; push.beginResult = beginResult;
    event.completion = completion;
    recorder.mediaEvent = &event; recorder.mediaControl = &control; recorder.framePush = &push;
    app->Recorder() = &recorder;
    app->FrameStepPlayback() = app->PlaybackActive() = 1;
    auto* previous = g_Block; g_Block = app.get();
    Context context{ app.get(), &recorder };
    HWND window = CreateWindowExW(0, L"MMSRecordingTeardownProbe", L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), &context);
    Check(window != nullptr, "create a hidden message-only teardown probe");
    app->state.hwnd = window;
    if (reenter) Check(PostMessageW(window, kReenter, 0, 0) != FALSE, "queue a nested stop during event drain");
    if (quit) PostQuitMessage(37);
    bool completed = true;
    const auto began = GetTickCount64();
    const bool result = TeardownDShowGraph(&recorder, &completed);
    Check(GetTickCount64() - began < 5000, "teardown does not wait for a stopped or failed source");
    const bool expected = running && SUCCEEDED(beginResult) && !quit && completion == EC_COMPLETE;
    Check(result == expected && completed == expected, "only a real completion event reports successful recording");
    Check(control.stops == 1 && push.begins == 1, "one teardown ends and stops the source exactly once");
    Check(!recorder.mediaEvent && !recorder.mediaControl && !recorder.framePush,
          "production teardown releases all recording interfaces");
    Check(event.additions == 1 && event.releases == 2 && event.references == 0 &&
          control.additions == 1 && control.releases == 2 && control.references == 0 && push.releases == 1,
          "local COM references survive callbacks and release exactly once after recorder ownership");
    if (!running || FAILED(beginResult))
        Check(event.reads == 0 && event.frees == 0, "partial and failed graphs do not await an event that cannot arrive");
    else if (quit)
        Check(event.reads == 1 && event.frees == 0, "quit exits event polling without freeing nonexistent parameters");
    else
        Check(event.reads == 2 && event.frees == 1, "a temporarily empty queue is polled again and terminal parameters are freed once");
    MSG message{};
    if (quit) {
        Check(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && message.wParam == 37,
              "the event drain preserves WM_QUIT and its exit code");
    } else {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    if (reenter) Check(context.nested == 1 && context.closes == 1,
        "real dispatch coalesces close requests and runs one deferred close");
    Check(!recording_state::Guard::Active(&recorder, recording_state::Operation::GraphTeardown),
          "stop guard is released after normal, partial, failed and quit teardown");
    DestroyWindow(window);
    app->Recorder() = nullptr;
    g_Block = previous;
}
}

int main() {
    SetEnvironmentVariableW(L"MMS_UI_METRICS", L"recording-entry-probe");
    Check(SUCCEEDED(CoInitialize(nullptr)), "initialize COM for teardown peers");
    WNDCLASSW definition{};
    definition.lpfnWndProc = Probe;
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.lpszClassName = L"MMSRecordingTeardownProbe";
    Check(RegisterClassW(&definition) != 0, "register hidden teardown probe");
    StoppedEntries(recording_state::Operation::Finish);
    StoppedEntries(recording_state::Operation::GraphTeardown);
    GraphCase(true, true, S_OK, EC_COMPLETE, false);
    GraphCase(false, true, S_OK, EC_ERRORABORT, false);
    GraphCase(false, false, S_OK, EC_COMPLETE, false);
    GraphCase(false, true, E_FAIL, EC_COMPLETE, false);
    GraphCase(false, true, S_OK, EC_COMPLETE, true);
    bool complete = true;
    Check(!TeardownDShowGraph(nullptr, &complete) && !complete, "null recorder teardown is a safe incomplete no-op");
    UnregisterClassW(definition.lpszClassName, definition.hInstance);
    CoUninitialize();
    std::printf("recording teardown: %u checks, 0 failures\n", checks);
    return 0;
}
