// Exercise production retry/SEH control flow and the encoder pixel contract.
// Default checks use no vendor driver or MP4 output. --orientation also encodes
// and decodes real MP4s through software, covering hardware's NV12 input format.
#include "../src/app/mf_h264_recorder.cpp"
#include <MinHook.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <array>
#include <string_view>
#include <tuple>

using namespace mikumikustudio;
namespace {
int checks = 0, opens = 0, releases = 0, notices = 0, observedGroup = -1;
int audioStreams = 0;
DWORD sampleBytes = 0;
enum class Fault { None, BeginException, BeginFailure, SettingsBefore, SettingsAfter, Release };
Fault fault = Fault::None;
HRESULT writeResult = S_OK;
int frameWrites = 0, finalizations = 0, frameErrors = 0;
DShowRecorder* recordingInError = nullptr;
bool failSoftware = false, probeFails = false;
bool captureFrame = false;
std::vector<unsigned char> capturedFrame;
LONG capturedStride = 0;
void (*storageWriterReleaseObserver)() = nullptr;
std::filesystem::path testOutputPath;
bool writeEncoderFixture = false;
HRESULT finalizeResult = S_OK;
decltype(&MoveFileExW) realMoveOutput = nullptr;
std::filesystem::path publicationTarget, displacedFixture;
unsigned publicationArrivals = 0;
bool denyPublication = false;
constexpr char lateVideo[] = "a video arrived while the export was publishing";
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", name); std::exit(1); }
}
BOOL WINAPI ArriveAtPublish(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (to && !publicationTarget.empty() && publicationTarget.native() == to && publicationArrivals++ == 0) {
        if (denyPublication) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (std::filesystem::exists(publicationTarget))
            Check(realMoveOutput(to, displacedFixture.c_str(), MOVEFILE_WRITE_THROUGH), "preserve the fixture's old target before installing the late arrival");
        const HANDLE late = CreateFileW(to, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(late != INVALID_HANDLE_VALUE, "create an independent late output at publication");
        DWORD written = 0;
        Check(WriteFile(late, lateVideo, sizeof(lateVideo) - 1, &written, nullptr) && written == sizeof(lateVideo) - 1,
              "write all bytes to the late output");
        Check(CloseHandle(late), "close the late output before the real publication call");
    }
    return realMoveOutput(from, to, flags);
}
void Crash() { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }
struct Writer final : IMFSinkWriter {
    bool hardware = false, begun = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** result) override { *result=nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { if(storageWriterReleaseObserver) storageWriterReleaseObserver(); ++releases; if (hardware && fault==Fault::Release) Crash(); return 1; }
    HRESULT STDMETHODCALLTYPE AddStream(IMFMediaType*, DWORD* id) override { *id=1; ++audioStreams; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetInputMediaType(DWORD, IMFMediaType* type, IMFAttributes*) override {
        if(captureFrame){UINT32 stride=0;Check(SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,&stride)),"input type declares its row order");capturedStride=static_cast<LONG>(stride);}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginWriting() override {
        if ((hardware && fault==Fault::BeginException) || (!hardware && failSoftware)) Crash();
        if (hardware && (fault==Fault::BeginFailure || fault==Fault::Release)) return E_FAIL;
        begun=true; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WriteSample(DWORD, IMFSample* sample) override {
        ++frameWrites;
        if (FAILED(writeResult)) return writeResult;
        if(captureFrame){
            IMFMediaBuffer* buffer=nullptr;Check(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)),"read actual encoder input");
            BYTE* bytes=nullptr;DWORD size=0;Check(SUCCEEDED(buffer->Lock(&bytes,nullptr,&size)),"lock actual encoder input");
            capturedFrame.assign(bytes,bytes+size);buffer->Unlock();buffer->Release();
        }
        return sample->GetTotalLength(&sampleBytes);
    }
    HRESULT STDMETHODCALLTYPE SendStreamTick(DWORD,LONGLONG) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PlaceMarker(DWORD,LPVOID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE NotifyEndOfSegment(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Flush(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Finalize() override { ++finalizations; return finalizeResult; }
    HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD,REFGUID,REFIID,LPVOID*) override { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetStatistics(DWORD,MF_SINK_WRITER_STATISTICS*) override { return E_NOTIMPL; }
} writers[2];
HRESULT Open(IMFSinkWriter** out,const wchar_t* path,bool hardware,std::int32_t,
             std::int32_t,float,std::uint32_t,bool nv12,DWORD* stream) noexcept {
    Check(nv12==hardware,"retry chooses the matching pixel format");
    ++opens;
    if (writeEncoderFixture) {
        std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        file << "new encoder output";
        Check(file.good(), "the encoder actually opens and truncates its supplied path");
    }
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
int WINAPI FrameError(HWND, LPCWSTR, LPCWSTR, UINT) {
    ++frameErrors;
    // A modal error pumps messages that can query the recorder before stopping it.
    if (recordingInError) IsBuiltInH264Recording(recordingInError);
    return IDOK;
}
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
    const bool ok=StartBuiltInH264Recording(&recorder,testOutputPath.c_str(),16,16,30.f,audio,true,1000000,nullptr,1);
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
void Conversion() {
    // A two-by-two block has four planar luma bytes and one interleaved UV
    // pair. Fixed expected values pin BT.709 studio range and BGRA ordering.
    struct Color { unsigned char b,g,r,y,u,v; };
    const Color colors[]={{0,0,0,16,128,128},{255,255,255,234,128,128},
        {0,0,255,62,102,240},{0,255,0,172,41,26},{255,0,0,32,239,117}};
    for(const auto& color:colors){
        std::array<unsigned char,16> bgra{};std::array<unsigned char,8> nv12{};nv12.fill(0xA7);
        for(unsigned pixel=0;pixel<4;++pixel){bgra[pixel*4]=color.b;bgra[pixel*4+1]=color.g;bgra[pixel*4+2]=color.r;bgra[pixel*4+3]=17;}
        ConvertBgraToNv12(bgra.data(),2,2,nv12.data());
        Check(nv12[0]==color.y&&nv12[1]==color.y&&nv12[2]==color.y&&nv12[3]==color.y&&nv12[4]==color.u&&nv12[5]==color.v,
            "CPU NV12 conversion preserves BGRA ordering and BT.709 studio-range colors");
        Check(nv12[6]==0xA7&&nv12[7]==0xA7,"NV12 conversion writes only the declared planar buffer");
        const auto first=nv12;for(unsigned pixel=0;pixel<4;++pixel)bgra[pixel*4+3]=255;
        ConvertBgraToNv12(bgra.data(),2,2,nv12.data());
        Check(nv12==first,"opaque video color conversion does not interpret source alpha as RGB");
    }
    std::array<unsigned char,16> rows{0,0,255,255,0,0,255,255,255,0,0,255,255,0,0,255};
    std::array<unsigned char,6> nv12{};ConvertBgraToNv12(rows.data(),2,2,nv12.data());
    Check(nv12[0]==32&&nv12[1]==32&&nv12[2]==62&&nv12[3]==62&&nv12[4]==171&&nv12[5]==178,
        "NV12 reverses bottom-up source luma rows and averages chroma over each two-by-two block");
}
void HealthyFrame(bool hardware) {
    fault=Fault::None;failSoftware=false;opens=releases=notices=0;sampleBytes=0;
    g_hardwareEncoderUnsafe=false;g_hardwareFallbackNoticeShown=false;DShowRecorder recorder{};
    Check(StartBuiltInH264Recording(&recorder,testOutputPath.c_str(),16,16,30.f,nullptr,hardware,1000000,nullptr,1),
        "healthy encoder startup completes without a fallback");
    Check(opens==1&&!notices&&!g_hardwareEncoderUnsafe&&gRecordings.at(&recorder)->nv12Input==hardware,
        "hardware keeps NV12 and explicit software keeps RGB32 on healthy startup");
    std::array<unsigned char,16*16*4> bgra{};
    Check(WriteBuiltInH264Frame(&recorder,bgra.data(),bgra.size())&&
        sampleBytes==(hardware?16u*16u*3u/2u:unsigned(bgra.size()))&&gRecordings.at(&recorder)->frameIndex==1,
        "production frame writer submits the matching pixel format and one complete timestamped frame");
    Check(FinishBuiltInH264Recording(&recorder,1)&&!IsBuiltInH264Recording(&recorder)&&releases==1,
        "healthy writer finalizes the actual completed frame and releases once");
}
void FrameWriteFailure() {
    DShowRecorder recorder{};
    auto recording = std::make_unique<Recording>();
    recording->width = 16;
    recording->height = 16;
    std::array<unsigned char, 16 * 16 * 4> pixels{};
    recording->frameBytes = pixels.size();
    recording->frameDuration = 333333;
    recording->writer = &writers[0];
    gRecordings.emplace(&recorder, std::move(recording));
    fault = Fault::None;
    frameWrites = frameErrors = finalizations = releases = 0;
    recordingInError = &recorder;
    writeResult = E_ACCESSDENIED;
    Check(!WriteBuiltInH264Frame(&recorder, pixels.data(), pixels.size()),
          "write failure returns without entering a reentrant modal loop");
    Check(frameErrors == 0 && IsBuiltInH264Recording(&recorder),
          "the caller can query and stop the failed recording");
    writeResult = E_FAIL;
    Check(!WriteBuiltInH264Frame(&recorder, pixels.data(), pixels.size()) && frameWrites == 1,
          "a failed recording rejects further frames without calling the encoder");
    std::int32_t failure = 0;
    Check(!FinishBuiltInH264Recording(&recorder, 1, &failure) && failure == E_ACCESSDENIED,
          "cleanup returns the original write error to the caller");
    Check(!IsBuiltInH264Recording(&recorder) && frameErrors == 0 &&
          finalizations == 0 && releases == 1,
          "cleanup releases the failed writer once without retrying it or opening a dialog");
    recordingInError = nullptr;
    writeResult = S_OK;
}
void InvalidAudio() {
    fault=Fault::None;failSoftware=false;opens=releases=notices=0;
    g_hardwareEncoderUnsafe=false;g_hardwareFallbackNoticeShown=false;
    DShowRecorder recorder{};WaveAudioContext audio{};
    wcscpy_s(audio.path,L"test-only-invalid-audio.raw");audio.dataSize=4;
    Check(!StartBuiltInH264Recording(&recorder,testOutputPath.c_str(),16,16,30.f,&audio,true,1000000,nullptr,1)&&
        !opens&&!g_hardwareEncoderUnsafe&&!IsBuiltInH264Recording(&recorder),
        "invalid source audio refuses before creating an encoder and never disables hardware");
}

void OutputProtection() {
    namespace fs = std::filesystem;
    const auto root = testOutputPath.parent_path() / "publish";
    Check(fs::create_directory(root), "create a fresh output-protection fixture directory");
    const auto destination = root / "existing.mp4";
    constexpr char original[] = "original video bytes";
    constexpr char encoded[] = "new encoder output";
    { std::ofstream file(destination, std::ios::binary); file << original; Check(file.good(), "seed destination"); }
    const auto read = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    DShowRecorder recorder{};
    writeEncoderFixture = true;
    const auto start = [&](const fs::path& path, bool hardware = false) {
        return StartBuiltInH264Recording(&recorder, path.c_str(), 16, 16, 30.f,
                                        nullptr, hardware, 1000000, nullptr, 1);
    };
    const auto onlyDestination = [&] {
        return std::distance(fs::directory_iterator(root), fs::directory_iterator()) == 1;
    };
    g_hardwareEncoderUnsafe = false;
    fault = Fault::BeginFailure;
    failSoftware = true;
    Check(!start(destination, true), "both encoder starts fail after opening the output");
    Check(read(destination) == original && onlyDestination(),
          "both startup failures preserve the previous video and remove temporary output");
    failSoftware = false;
    fault = Fault::None;

    const std::array<unsigned char, 16 * 16 * 4> pixels{};
    const auto write = [&] { return WriteBuiltInH264Frame(&recorder, pixels.data(), pixels.size()); };
    std::int32_t failure = 0;
    Check(start(destination) && read(destination) == original, "start keeps the previous video visible");
    writeResult = E_ACCESSDENIED;
    Check(!write() && !FinishBuiltInH264Recording(&recorder, 1, &failure) &&
          failure == E_ACCESSDENIED && read(destination) == original && onlyDestination(),
          "frame failure preserves the previous video and cleans up");
    writeResult = S_OK;

    Check(start(destination) && write(), "write a frame before the finalization failure");
    finalizeResult = E_FAIL;
    Check(!FinishBuiltInH264Recording(&recorder, 1, &failure) && failure == E_FAIL &&
          read(destination) == original && onlyDestination(),
          "finalization failure preserves the previous video and cleans up");
    finalizeResult = S_OK;

    Check(start(destination) && write(), "write a frame before cancellation");
    Check(!FinishBuiltInH264Recording(&recorder, 0, &failure) && failure == 0 &&
          read(destination) == original && onlyDestination(), "cancel preserves the previous video");

    Check(start(destination) && write(), "write the replacement before publication");
    HANDLE lock = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "hold the original destination against replacement");
    const bool published = FinishBuiltInH264Recording(&recorder, 1, &failure);
    CloseHandle(lock);
    Check(!published && failure != 0 && read(destination) == original && onlyDestination(),
          "denied replacement preserves the previous video and returns an error");

    const auto parked = root / "previous.mp4";
    constexpr char foreign[] = "a different video appeared during export";
    Check(start(destination) && write(), "encode before an external replacement of the destination");
    fs::rename(destination, parked);
    { std::ofstream file(destination, std::ios::binary); file << foreign; Check(file.good(), "replace the destination during encoding"); }
    const bool replacedForeign = FinishBuiltInH264Recording(&recorder, 1, &failure);
    Check(!replacedForeign && failure == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) &&
          read(destination) == foreign && read(parked) == original &&
          std::distance(fs::directory_iterator(root), fs::directory_iterator()) == 2,
          "publication refuses a different destination and cleans up temporary output");
    Check(fs::remove(destination), "retire the owned foreign output fixture");
    fs::rename(parked, destination);

    Check(start(destination) && write(), "encode before an in-place edit of the destination");
    { std::ofstream file(destination, std::ios::binary | std::ios::app); file << " changed"; Check(file.good(), "edit the original destination in place"); }
    Check(!FinishBuiltInH264Recording(&recorder, 1, &failure) && failure == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) &&
          read(destination) == std::string(original) + " changed" && onlyDestination(),
          "publication refuses an in-place edit and cleans up temporary output");

    Check(start(destination) && write(), "encode before the existing destination disappears");
    fs::rename(destination, parked);
    Check(!FinishBuiltInH264Recording(&recorder, 1, &failure) && failure == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
          !fs::exists(destination) && read(parked) == std::string(original) + " changed" && onlyDestination(),
          "publication refuses a removed destination and leaves the moved file intact");
    fs::rename(parked, destination);

    Check(start(destination) && write() && FinishBuiltInH264Recording(&recorder, 1, &failure) &&
          failure == 0 && read(destination) == encoded && onlyDestination(),
          "retry replaces the previous video only after successful finalization");

    publicationTarget = destination;
    displacedFixture = root / "displaced.mp4";
    publicationArrivals = 0;
    Check(MH_CreateHook(reinterpret_cast<void*>(&MoveFileExW), reinterpret_cast<void*>(&ArriveAtPublish),
                        reinterpret_cast<void**>(&realMoveOutput)) == MH_OK &&
          MH_EnableHook(reinterpret_cast<void*>(&MoveFileExW)) == MH_OK, "observe the final publication call");
    std::wstring retained;
    denyPublication = true;
    Check(start(destination) && write(), "encode before a denied publication with a free target name");
    Check(!FinishBuiltInH264Recording(&recorder, 1, &failure, &retained) &&
          failure == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && retained.empty() &&
          publicationArrivals == 1 && read(destination) == encoded && onlyDestination(),
          "denied publication restores the old video when the target name is free");

    denyPublication = false;
    publicationArrivals = 0;
    Check(start(destination) && write(), "encode before a late arrival at the publication boundary");
    const bool overwroteLate = FinishBuiltInH264Recording(&recorder, 1, &failure, &retained);
    Check(MH_RemoveHook(reinterpret_cast<void*>(&MoveFileExW)) == MH_OK, "remove the publication observer");
    publicationTarget.clear();
    Check(!overwroteLate && failure != 0 && publicationArrivals == 1 && read(destination) == lateVideo,
          "final publication preserves a video arriving after destination verification");
    const fs::path previous(retained);
    Check(previous.parent_path() == root && previous.extension() == L".bak" && read(previous) == encoded &&
          std::distance(fs::directory_iterator(root), fs::directory_iterator()) == 2,
          "the reported previous video is readable and only the two user videos remain after conflict");
    Check(start(destination) && write() && FinishBuiltInH264Recording(&recorder, 1, &failure) &&
          failure == 0 && read(destination) == encoded && read(previous) == encoded,
          "an explicit retry can replace the new target and keeps the previously retained video");
    Check(fs::remove(previous), "retire the owned previous-video fixture");

    const auto fresh = root / "fresh.mp4";
    Check(start(fresh) && write(), "encode to an unclaimed path before another writer claims it");
    { std::ofstream file(fresh, std::ios::binary); file << foreign; Check(file.good(), "claim the new output path during encoding"); }
    Check(!FinishBuiltInH264Recording(&recorder, 1, &failure) && failure != 0 &&
          read(fresh) == foreign && std::distance(fs::directory_iterator(root), fs::directory_iterator()) == 2,
          "publication to a new path cannot replace a late arrival");
    Check(fs::remove(fresh), "retire the owned late-arrival fixture");
    Check(start(fresh) && write() && !fs::exists(fresh), "a new output appears only after finalization");
    Check(!FinishBuiltInH264Recording(&recorder, 0, &failure) && failure == 0 &&
          read(fresh) == encoded, "a cancelled export to a new path retains its finalized partial video");
    writeEncoderFixture = false;
}

#include "h264_orientation_checks.hpp"
#include "h264_system_encoder_checks.inc"
#include "mf_h264_probe_storage_checks.inc"
}
int main(int argc,char** argv) {
    const bool systemEncoder = argc == 3 && std::string(argv[1]) == "--system-encoder";
    if (systemEncoder) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::puts("system-encoder: initializing COM and Media Foundation");
    }
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialized");
    Check(SUCCEEDED(MFStartup(MF_VERSION)),"MF initialized for encoder input checks");
    if (systemEncoder) {
        RunSystemEncoderCheck(std::filesystem::absolute(argv[2]));
        MFShutdown();
        CoUninitialize();
        return 0;
    }
    if(argc>=2&&std::string(argv[1])=="--probe-storage") {
        Check(argc==3,"probe storage requires an explicit absolute fresh output directory");
        probe_storage::Run(std::filesystem::path(argv[2]));
        MFShutdown();CoUninitialize();std::printf("h264 probe storage: %d checks passed\n",checks);return 0;
    }
    H264InputOrientation();
    if(argc==3&&std::string(argv[1])=="--orientation"){
        H264EncodedOrientation(std::filesystem::absolute(argv[2]));
        MFShutdown();CoUninitialize();std::printf("h264 orientation: %d checks passed\n",checks);return 0;
    }
    Check(MH_Initialize()==MH_OK,"hooks initialized");
    Hook(OpenH264SinkWriterGuarded,Open); Hook(ApplyConfiguredEncoderSettings,Apply);
    Hook(ReportHardwareFallbackOnce,Notice); Hook(CreateEncoderProbeGuarded,Probe);
    Hook(CreateWindowExW,SettingsWindow);
    Hook(MessageBoxW,FrameError);
    Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"hooks enabled");
    const auto fixture = std::filesystem::current_path() /
        (L"MMS-h264-output-" + std::to_wstring(GetCurrentProcessId()));
    Check(std::filesystem::create_directory(fixture), "create a fresh encoder fixture directory");
    testOutputPath = fixture / L"video.mp4";
    Conversion();HealthyFrame(false);HealthyFrame(true);FrameWriteFailure();InvalidAudio();
    for(Fault f:{Fault::BeginException,Fault::BeginFailure,Fault::SettingsBefore,Fault::SettingsAfter,Fault::Release}) Case(f);
    Case(Fault::BeginException,true);
    OutputProtection();
    g_hardwareEncoderUnsafe=true;g_hardwareFallbackNoticeShown=true;ResetH264HardwareEncoderState();
    Check(!g_hardwareEncoderUnsafe&&!g_hardwareFallbackNoticeShown,"explicit user retry clears both unsafe and fallback-notice latches");
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
    MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); MFShutdown(); CoUninitialize();
    std::printf("mf_h264_startup_tests: %d checks passed\n",checks);
}
