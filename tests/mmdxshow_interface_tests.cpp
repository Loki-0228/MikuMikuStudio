// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include <initguid.h>
#include "../src/mmdxshow/mmdxshow.hpp"
#include <amvideo.h>
#include <uuids.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>

static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

struct RejectingInput final : IMemInputPin {
    unsigned deliveries = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator*, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Receive(IMediaSample*) override { ++deliveries; return E_FAIL; }
    HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample**, long, long*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_FALSE; }
};

static void DeliveryFailure(IClassFactory* factory) {
    IBaseFilter* filter = nullptr;
    Check(factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&filter)) == S_OK,
          "create isolated failed-delivery source");
    IPushSource* push = nullptr;
    Check(filter->QueryInterface(MMDXSHOW_IID_IPushSource, reinterpret_cast<void**>(&push)) == S_OK,
          "failed-delivery source exposes push interface");
    auto* pin = static_cast<CPushSourceDIBSq*>(filter)->m_pPin;
    IMemAllocator* allocator = nullptr;
    Check(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
        IID_IMemAllocator, reinterpret_cast<void**>(&allocator)) == S_OK, "allocate ordinary DirectShow sample");
    ALLOCATOR_PROPERTIES requested{1, 16, 1, 0}, actual{};
    Check(allocator->SetProperties(&requested, &actual) == S_OK && allocator->Commit() == S_OK,
          "prepare ordinary sample allocator");
    RejectingInput downstream;
    pin->m_pAllocator = allocator; pin->m_pInputPin = &downstream;
    BITMAPINFOHEADER info{}; info.biSize = sizeof info; info.biWidth = info.biHeight = 2;
    info.biPlanes = 1; info.biBitCount = 32; info.biSizeImage = 16;
    DWORD pixels[4]{};
    Check(push->SetBitmapInfo(&info, 40, 29.97f) == S_OK &&
          push->StartStreaming(reinterpret_cast<DWORD_PTR>(pixels)) == S_OK, "queue frame before encoder refusal");
    static_cast<CAMThread*>(pin)->DoBufferProcessingLoop();
    BYTE acknowledged = 1;
    Check(downstream.deliveries == 1 && push->GetStreamingState(&acknowledged) == E_FAIL &&
          push->StartStreaming(reinterpret_cast<DWORD_PTR>(pixels)) == E_FAIL,
          "downstream refusal publishes a terminal state instead of waiting for another acknowledgement");
    pin->m_pInputPin = nullptr;
    push->Release(); filter->Release();
}

int wmain(int argc, wchar_t** argv) {
    Check(argc == 2, "MMDxShow DLL path supplied");
    CoInitialize(nullptr);
    HMODULE dll = LoadLibraryW(argv[1]);
    Check(dll != nullptr, "load rebuilt MMDxShow");
    auto getClass = reinterpret_cast<HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(dll, "DllGetClassObject"));
    Check(getClass != nullptr, "resolve class factory");
    IClassFactory* factory = nullptr;
    Check(SUCCEEDED(getClass(MMDXSHOW_CLSID_PushSourceDIBSq, IID_IClassFactory,
                            reinterpret_cast<void**>(&factory))), "obtain filter factory");
    IBaseFilter* filter = nullptr;
    Check(SUCCEEDED(factory->CreateInstance(nullptr, IID_IBaseFilter,
                    reinterpret_cast<void**>(&filter))), "query native IBaseFilter subobject");
    IPushSource* push = nullptr;
    Check(SUCCEEDED(filter->QueryInterface(MMDXSHOW_IID_IPushSource,
                    reinterpret_cast<void**>(&push))), "query native IPushSource subobject");
    IAMovieSetup* setup = nullptr;
    Check(SUCCEEDED(filter->QueryInterface(IID_IAMovieSetup,
                    reinterpret_cast<void**>(&setup))), "query IAMovieSetup subobject");
    setup->Release();
    IEnumPins* pins = nullptr;
    Check(SUCCEEDED(filter->EnumPins(&pins)), "enumerate output pins");
    IPin* pin = nullptr;
    Check(pins->Next(1, &pin, nullptr) == S_OK, "obtain typed IPin subobject");
    IQualityControl* quality = nullptr;
    const auto qualityHr = pin->QueryInterface(IID_IQualityControl,
                    reinterpret_cast<void**>(&quality));
    Check(SUCCEEDED(qualityHr), "query pin's IQualityControl subobject");
    quality->Release();
    float version = 0.f;
    Check(push->GetRate(&version) == S_OK && version > 1.01f, "version handshake");
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = header.biHeight = 2;
    header.biPlanes = 1;
    header.biBitCount = 32;
    for (float fps : {0.f, -.5f, 241.f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()})
        Check(push->SetBitmapInfo(&header, 0, fps) == E_INVALIDARG, "reject unusable frame rates");
    Check(push->SetBitmapInfo(&header, 0, .5f) == S_OK, "sub-one FPS does not divide by zero");
    Check(push->SetBitmapInfo(&header, 0, 29.97f) == S_OK, "accept fractional FPS");
    IEnumMediaTypes* mediaTypes = nullptr;
    Check(pin->EnumMediaTypes(&mediaTypes) == S_OK, "enumerate configured frame timing");
    AM_MEDIA_TYPE* mediaType = nullptr;
    Check(mediaTypes->Next(1, &mediaType, nullptr) == S_OK && mediaType &&
          reinterpret_cast<VIDEOINFOHEADER*>(mediaType->pbFormat)->AvgTimePerFrame == 333667,
          "the published AVI media type retains 29.97 FPS");
    CoTaskMemFree(mediaType->pbFormat); CoTaskMemFree(mediaType); mediaTypes->Release();
    Check(push->SetBitmapInfo(&header, 0, 30.f) == S_OK, "set bitmap fields");
    BYTE state = 0;
    Check(push->GetStreamingState(&state) == S_OK && state == 1, "frame acknowledgement field");
    DWORD pixels[4]{};
    Check(push->StartStreaming(reinterpret_cast<DWORD_PTR>(pixels)) == S_OK,
          "pass native-width frame pointer");
    Check(push->GetStreamingState(&state) == S_OK && state == 0, "frame state changes after push");
    Check(push->BeginStreaming() == S_OK && push->GetRate(&version) == E_FAIL,
          "stream-end latch uses the same typed field as the pin");
    pin->Release();
    pins->Release();
    push->Release();
    filter->Release();
    DeliveryFailure(factory);
    factory->Release();
    auto canUnload = reinterpret_cast<HRESULT (STDAPICALLTYPE*)()>(
        GetProcAddress(dll, "DllCanUnloadNow"));
    Check(canUnload && canUnload() == S_OK, "typed destruction releases filter and pin");
    FreeLibrary(dll);
    CoUninitialize();
    std::puts("MMDxShow interface and streaming-state regressions passed");
}
