// Exercise production retry/SEH control flow with a failing encoder boundary.
// No vendor driver or output MP4 is touched by these fault-injection cases.
#include "../src/app/mf_h264_recorder.cpp"
#include <MinHook.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace mikumikustudio;
namespace {
int checks = 0, opens = 0, releases = 0, notices = 0, observedGroup = -1;
int audioStreams = 0;
enum class Fault { None, BeginException, BeginFailure, SettingsBefore, SettingsAfter, Release };
Fault fault = Fault::None;
bool failSoftware = false, probeFails = false;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", name); std::exit(1); }
}
void Crash() { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }
struct Writer final : IMFSinkWriter {
    bool hardware = false, begun = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** result) override { *result=nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; if (hardware && fault==Fault::Release) Crash(); return 1; }
    HRESULT STDMETHODCALLTYPE AddStream(IMFMediaType*, DWORD* id) override { *id=1; ++audioStreams; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetInputMediaType(DWORD, IMFMediaType*, IMFAttributes*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginWriting() override {
        if ((hardware && fault==Fault::BeginException) || (!hardware && failSoftware)) Crash();
        if (hardware && (fault==Fault::BeginFailure || fault==Fault::Release)) return E_FAIL;
        begun=true; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WriteSample(DWORD, IMFSample*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SendStreamTick(DWORD,LONGLONG) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PlaceMarker(DWORD,LPVOID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE NotifyEndOfSegment(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Flush(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Finalize() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD,REFGUID,REFIID,LPVOID*) override { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetStatistics(DWORD,MF_SINK_WRITER_STATISTICS*) override { return E_NOTIMPL; }
} writers[2];
HRESULT Open(IMFSinkWriter** out,const wchar_t*,bool hardware,std::int32_t,
             std::int32_t,float,std::uint32_t,bool nv12,DWORD* stream) noexcept {
    Check(nv12==hardware,"retry chooses the matching pixel format");
    ++opens;
    auto& writer=writers[hardware?1:0]; writer.hardware=hardware; writer.begun=false;
    *out=&writer; *stream=0; return S_OK;
}
ApplyResult Apply(IMFSinkWriter* writer,DWORD,bool hardware,std::uint32_t,unsigned char) {
    auto* fake=static_cast<Writer*>(writer);
    if (hardware && ((fault==Fault::SettingsBefore && !fake->begun) ||
                     (fault==Fault::SettingsAfter && fake->begun))) Crash();
    observedGroup=hardware?1:0;
    ApplyResult result;
    result.encoderFound=fake->begun;
    return result;
}
void Notice(void*,unsigned char) { ++notices; }
bool Probe(bool hardware,std::int32_t,std::int32_t,float,std::uint32_t,EncoderProbe&) noexcept {
    return !(hardware && probeFails);
}
HWND WINAPI SettingsWindow(DWORD,LPCWSTR name,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID param) {
    Check(wcscmp(name,L"MMS.H264EncoderSettings")==0,"intercept only the settings window");
    observedGroup=static_cast<EncoderSettingsDialog*>(param)->group;
    return nullptr; // inspect the state without displaying a window or entering its loop
}
template<class F> void Hook(F target,F replacement) {
    Check(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),nullptr)==MH_OK,"create test boundary hook");
}
void Case(Fault injected,bool softwareFailure=false,const WaveAudioContext* audio=nullptr) {
    fault=injected; failSoftware=softwareFailure; opens=releases=notices=audioStreams=0;
    observedGroup=-1; g_hardwareEncoderUnsafe=false;
    DShowRecorder recorder{};
    if(audio) std::printf("audio format=%u channels=%u sampleRate=%u align=%u bits=%u path=%ls size=%d offset=%d\n",
        audio->format.wFormatTag,audio->format.nChannels,audio->format.nSamplesPerSec,audio->format.nBlockAlign,
        audio->format.wBitsPerSample,audio->path,audio->dataSize,audio->dataOffset);
    const bool ok=StartBuiltInH264Recording(&recorder,L"unused.mp4",16,16,30.f,audio,true,1000000,nullptr,1);
    std::printf("case=%d audio=%d softwareFailure=%d ok=%d opens=%d releases=%d notices=%d group=%d\n",
        static_cast<int>(injected),audio!=nullptr,softwareFailure,ok,opens,releases,notices,observedGroup);
    Check(ok!=softwareFailure,"startup returns the final attempt result");
    Check(opens==2 && releases==(softwareFailure?2:1),"failed hardware writer is released and software is tried once");
    Check(g_hardwareEncoderUnsafe,"driver failure blocks hardware until explicit retry");
    Check(notices==(softwareFailure?0:1),"fallback is announced only after successful startup");
    Check(IsBuiltInH264Recording(&recorder)==ok,"failed attempts are never published as recordings");
    if(ok) {
        auto& r=*gRecordings.at(&recorder);
        Check(!r.nv12Input && observedGroup==0,"software retry uses RGB32 and software settings");
        if(audio) Check(r.hasAudio && r.audioStream==1 && audioStreams==2,"audio is attached again on the software retry");
        gRecordings.erase(&recorder);
    }
}
}
int main() {
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialized");
    Check(MH_Initialize()==MH_OK,"hooks initialized");
    Hook(OpenH264SinkWriterGuarded,Open); Hook(ApplyConfiguredEncoderSettings,Apply);
    Hook(ReportHardwareFallbackOnce,Notice); Hook(CreateEncoderProbeGuarded,Probe);
    Hook(CreateWindowExW,SettingsWindow);
    Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"hooks enabled");
    for(Fault f:{Fault::BeginException,Fault::BeginFailure,Fault::SettingsBefore,Fault::SettingsAfter,Fault::Release}) Case(f);
    Case(Fault::BeginException,true);
    const auto path=std::filesystem::current_path()/
        (L"MMS-h264-audio-"+std::to_wstring(GetCurrentProcessId())+L".raw");
    {std::ofstream out(path,std::ios::binary); out << "1234";Check(out.good(),"create owned audio fixture");}
    WaveAudioContext audio{}; wcscpy_s(audio.path,path.c_str()); audio.dataSize=4;
    audio.format={WAVE_FORMAT_PCM,1,44100,88200,2,16,0};
    Case(Fault::BeginException,false,&audio);
    std::filesystem::remove(path);
    for(bool hardware:{false,true}) for(bool blocked:{false,true}) for(bool failProbe:{false,true}) {
        g_hardwareEncoderUnsafe=blocked; probeFails=failProbe; observedGroup=-1;
        std::uint32_t bitrate=1000000;
        ShowH264EncoderSettingsDialog(nullptr,hardware,bitrate,16,16,30.f,1,nullptr);
        Check(observedGroup==(hardware&&!blocked&&!failProbe?1:0),"settings group follows the encoder actually probed");
    }
    MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); CoUninitialize();
    std::printf("mf_h264_startup_tests: %d checks passed\n",checks);
}
