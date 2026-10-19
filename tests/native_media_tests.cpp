#define NOMINMAX
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/wic_mov_recorder.hpp"
#include "mikumikustudio/wave_audio_context.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
using namespace mikumikustudio;
namespace fs = std::filesystem;
void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
void CheckResult(bool ok, const std::wstring& error, const char* why) {
    if (!ok) std::fwprintf(stderr, L"%s\n", error.c_str()); Check(ok, why);
}
void ProbeVideo(const fs::path& source) {
    auto* app = reinterpret_cast<MMDApp*>(static_cast<std::uintptr_t>(1));
    int w = 0, h = 0; double duration = 0; std::wstring error;
    CheckResult(media::OpenVideo(app, source.c_str(), w, h, duration, error, 1024), error, "video fixture open");
    Check(w > 0 && h > 0 && w <= 1024 && h <= 1024, "bounded preview dimensions");
    std::wprintf(L"VIDEO %s %dx%d %.3fs\n", source.filename().c_str(), w, h, duration);
    const size_t bytes = sizeof(BITMAPINFOHEADER) + size_t(w) * h * 4;
    for (double fraction : {.8, .1, .9, .0, .99, .4})
        CheckResult(media::VideoFrame(app, duration * fraction, error) != nullptr, error, "exact seek including reverse and EOF");
    const double target = duration * .7;
    auto* exact = media::VideoFrame(app, target, error);
    CheckResult(exact != nullptr, error, "reference export frame");
    std::vector<unsigned char> expected(reinterpret_cast<unsigned char*>(exact), reinterpret_cast<unsigned char*>(exact) + bytes);
    auto* initial = media::VideoFrame(app, 0, error);
    std::vector<unsigned char> snapshot(reinterpret_cast<unsigned char*>(initial), reinterpret_cast<unsigned char*>(initial) + bytes);
    // A worker must never overwrite a DIB still being uploaded by the renderer.
    auto* retained = media::VideoFrame(app, duration * .8, error, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(std::memcmp(retained, snapshot.data(), bytes) == 0, "published DIB remains immutable during background decode");
    std::vector<double> calls;
    for (int i = 0; i < 300; ++i) {
        const auto start = std::chrono::steady_clock::now();
        CheckResult(media::VideoFrame(app, duration * ((i * 37) % 100) / 100., error, false) != nullptr, error, "nonblocking scrub");
        calls.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const auto start = std::chrono::steady_clock::now();
    bool caughtUp = false;
    do {
        auto* latest = media::VideoFrame(app, target, error, false);
        CheckResult(latest != nullptr, error, "latest request catches up");
        if (std::memcmp(latest, expected.data(), bytes) == 0) { caughtUp = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(15));
    Check(caughtUp, "last scrub request matches exact export frame without further timeline movement");
    std::sort(calls.begin(), calls.end());
    std::printf("SCRUB calls=300 p95_ms=%.6f max_ms=%.6f catchup_ms=%.3f\n", calls[285], calls.back(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    Check(calls[285] < 20, "preview calls do not wait for GOP decoding");
    media::VideoFrame(app, duration * .9, error, false);
    media::ReleaseVideo(app);
    Check(!media::HasVideo(app), "close with decode pending");
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--video") == 0) { ProbeVideo(argv[2]); return 0; }
    Check(argc == 2, "fixture directory required");
    const fs::path directory = argv[1];
    Check(media::IsAudioPath(L"song.Mp3") && media::IsAudioPath(L"song.AAC") && !media::IsAudioPath(L"song.mp3.txt"), "exact audio extensions");
    Check(media::IsVideoPath(L"clip.MOV") && media::IsModernVideoPath(L"clip.Mp4") && !media::IsVideoPath(L"clip.mov.png"), "exact video extensions");
    WaveAudioContext audio{};
    for (const auto* extension : {L"mp3", L"aac", L"m4a", L"wma"}) {
        std::wstring playable, error;
        const auto source = directory / (std::wstring(L"tone.") + extension);
        CheckResult(media::PrepareAudio(&audio, source.c_str(), playable, error), error, "native audio decode");
        Check(playable != source.wstring() && fs::exists(playable), "decoded cache exists");
        Check(media::AudioSource(&audio) == source.wstring(), "source path preserved for PMM");
        std::ifstream input(fs::path(playable), std::ios::binary);
        std::array<unsigned char, 44> header{}; input.read(reinterpret_cast<char*>(header.data()), 44);
        Check(header[20] == 1 && header[34] == 16 && header[22] == 2, "cache PCM16 stereo");
        const auto bytes = fs::file_size(playable) - 44;
        std::printf("Decoded PCM bytes: %llu\n", static_cast<unsigned long long>(bytes));
        // WMA exposes its codec block delay; MP3/AAC containers trim closer to the source duration.
        const double minimum = std::wcscmp(extension, L"wma") == 0 ? .90 : .99;
        Check(bytes >= 48000 * 4 * minimum && bytes < 48000 * 4 * 1.1, "audio duration and channels");
        double power = 0; std::int16_t sample = 0; std::size_t count = 0;
        while (input.read(reinterpret_cast<char*>(&sample), 2)) { power += double(sample) * sample; ++count; }
        Check(count && std::sqrt(power / count) > 500, "non-silent decoded audio");
        input.close(); media::ReleaseAudio(&audio);
        Check(!fs::exists(playable), "owned cache removed after release");
        std::wprintf(L"PASS native audio %s\n", extension);
    }
    std::wstring playable, error;
    const auto wav = directory / L"tone.wav";
    Check(media::PrepareAudio(&audio, wav.c_str(), playable, error) && playable == wav.wstring(), "legacy PCM WAV remains direct");
    Check(!media::PrepareAudio(&audio, (directory / L"missing.mp3").c_str(), playable, error) && !error.empty(), "missing audio reports error");
    for (const auto* name : {L"h264-baseline.mp4", L"h264-main.mp4", L"h264-high.mp4", L"h264-high10.mp4", L"h264-4k.mp4"})
        ProbeVideo(directory / name);
    auto* app = reinterpret_cast<MMDApp*>(static_cast<std::uintptr_t>(1));
    for (const auto* name : {L"corners.mp4", L"corners.mov", L"h264-high10.mp4"}) {
        int w = 0, h = 0; double duration = 0;
        const auto source = directory / name;
        CheckResult(media::OpenVideo(app, source.c_str(), w, h, duration, error), error, "native video open");
        Check(w == 64 && h == 48 && duration >= 7.99 && duration <= 8.01, "video geometry and duration");
        for (double seconds : {0., 1.1, .1, 1.9, 6.1, 7.9, 2.1, 0.}) {
            auto* dib = media::VideoFrame(app, seconds, error);
            CheckResult(dib != nullptr, error, "forward reverse and EOF seek");
            auto* pixels = reinterpret_cast<unsigned char*>(dib + 1);
            auto* top = pixels + ((h - 1 - 8) * w + 8) * 4;
            auto* bottom = pixels + (8 * w + 8) * 4;
            const int channel = static_cast<int>(seconds) % 2 == 0 ? 2 : 1;
            std::printf("video t=%.1f top=%u,%u,%u bottom=%u,%u,%u\n", seconds,
                top[0],top[1],top[2],bottom[0],bottom[1],bottom[2]);
            Check(top[channel] > 235 && top[0] < 20, "frame time and top colour");
            Check(bottom[0] > 235 && bottom[1] < 20 && bottom[2] < 20, "RGB32 orientation bottom blue");
        }
        media::ReleaseVideo(app); Check(!media::HasVideo(app), "video release");
        std::wprintf(L"PASS native video %s\n", name);
    }
    {
        int w=0,h=0; double duration=0;
        CheckResult(media::OpenVideo(app,(directory/L"corners.mp4").c_str(),w,h,duration,error,32),error,
                    "open resized video preview");
        Check(w==32 && h==24,"preview conversion preserves aspect ratio and requested bound");
        for(double seconds : {0.,1.1,7.9,.1}) {
            auto* dib=media::VideoFrame(app,seconds,error);
            CheckResult(dib!=nullptr,error,"resized preview supports seeking");
            const auto* pixels=reinterpret_cast<const unsigned char*>(dib+1);
            const auto* top=pixels+((h-1-4)*w+4)*4;
            const auto* bottom=pixels+(4*w+4)*4;
            Check(top[int(seconds)%2==0?2:1]>230 && bottom[0]>230,"resized preview retains colour and orientation");
            const auto revision=media::VideoFrameRevision(app);
            Check(revision && media::VideoFrame(app,seconds,error)==dib && media::VideoFrameRevision(app)==revision,
                  "repeated display of the same frame reuses the upload revision");
        }
        const auto revision=media::VideoFrameRevision(app);
        CheckResult(media::OpenVideo(app,(directory/L"corners.mp4").c_str(),w,h,duration,error,32),error,"reload preview");
        Check(media::VideoFrameRevision(app)!=revision,"reload invalidates the previous texture revision");
        media::ReleaseVideo(app);
    }
    auto* recorder = reinterpret_cast<DShowRecorder*>(static_cast<std::uintptr_t>(1));
    std::vector<unsigned char> pixels(64 * 48 * 4);
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
        auto* p = pixels.data() + (y * 64 + x) * 4;
        p[3] = x < 16 ? 0 : x < 40 ? 128 : 255;
        p[y < 24 ? 0 : 2] = p[3]; // bottom blue, top red; already premultiplied
    }
    const auto output = directory / L"alpha 输出.mov";
    CheckResult(StartPngMovRecording(recorder, output.c_str(), 64, 48, 30, error), error, "start WIC PNG encoder");
    for (int i = 0; i < 30; ++i) Check(WritePngMovFrame(recorder, pixels.data(), pixels.size()), "write PNG frame");
    CheckResult(FinishPngMovRecording(recorder, 30, error), error, "finalize PNG MOV");
    Check(fs::file_size(output) > 1000, "MOV committed");
    const auto size = fs::file_size(output);
    Check(StartPngMovRecording(recorder, output.c_str(), 64, 48, 30, error), "restart for cancellation");
    Check(WritePngMovFrame(recorder, pixels.data(), pixels.size()), "cancel sample");
    AbortPngMovRecording(recorder);
    Check(fs::file_size(output) == size && !IsPngMovRecording(recorder), "cancellation preserves existing output");
    Check(StartPngMovRecording(recorder, (directory / L"incomplete.mov").c_str(), 64, 48, 30, error), "start incomplete export");
    Check(WritePngMovFrame(recorder, pixels.data(), pixels.size()), "write incomplete frame");
    Check(!FinishPngMovRecording(recorder, 30, error) && !fs::exists(directory / L"incomplete.mov"), "incomplete recording is not reported as success");
    std::puts("PASS WIC PNG/MOV encoding, cancellation, and incomplete export");
}
