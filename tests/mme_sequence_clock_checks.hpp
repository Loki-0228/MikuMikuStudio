// Real MME TIME/ELAPSEDTIME semantics, including effects that run while stopped.
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/global_key_layout.hpp"
namespace {
namespace clock_seq = mikumikustudio::image_sequence;
decltype(&clock_seq::WriteFrame) clockOriginalWrite = nullptr;
std::vector<float> effectClockSamples;
float clockOutputFps = 30;
float EffectClock(const char* parameter) {
    Check(!sequenceClockEffects.empty(), "real MME compiled the clock fixture");
    float value = 0;
    using GetFloat = HRESULT(WINAPI*)(void*, const char*, float*);
    auto* effect = sequenceClockEffects.front();
    const HRESULT result = d3dx::EffectMethod<GetFloat, 31>(effect)(effect, parameter, &value);
    Check(SUCCEEDED(result),
        "read actual MME effect time parameter");
    return value;
}
bool CaptureEffectClock(MMDApp* app, IDirect3DSurface9* surface, std::wstring& error) {
    const float time = EffectClock("UnboundTime"), elapsed = EffectClock("UnboundElapsed");
    std::printf("CLOCK image=%zu cursor=%.6f time=%.6f elapsed=%.6f\n",
        effectClockSamples.size(), app->PlaybackCursorSeconds(), time, elapsed);
    if (!effectClockSamples.empty()) {
        Check(std::abs(time - effectClockSamples.back() - 1.f / clockOutputFps) < .002f,
            "unsynchronized MME TIME advances by the output interval despite delayed writes");
        Check(std::abs(elapsed - 1.f / clockOutputFps) < .002f,
            "unsynchronized MME ELAPSEDTIME follows output FPS");
    }
    effectClockSamples.push_back(time);
    return clockOriginalWrite(app, surface, error);
}
void CheckMmeSequenceClock(MMDApp* app, const wchar_t* directory) {
    std::filesystem::create_directories(directory);
    Check(!sequenceClockEffects.empty(), "load a model with an automatic TIME effect");
    app->CameraMode() = 1; app->CurrentFrame() = 0;
    app->CameraKeys()[0].distance = -45; app->CameraKeys()[0].eye[1] = 10;
    app->CameraKeys()[0].fov = 30; app->CameraKeys()[0].parentModel = -1;
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    const float idleBefore = EffectClock("UnboundTime");
    Sleep(80); app->state.messageSeen = 1; FrameDriver(app);
    const float idleAfter = EffectClock("UnboundTime");
    Check(idleAfter > idleBefore + .03f, "fixture really advances while playback is stopped");
    Check(MH_CreateHook(reinterpret_cast<void*>(&clock_seq::WriteFrame), &CaptureEffectClock,
        reinterpret_cast<void**>(&clockOriginalWrite)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&clock_seq::WriteFrame)) == MH_OK,
        "observe real MME at each encoded image");
    for (float fps : {30.f, 60.f}) {
        effectClockSamples.clear(); clockOutputFps = fps;
        clock_seq::Options options; options.directory = directory;
        options.prefix = L"clock_" + std::to_wstring(int(fps)) + L"_";
        options.width = 64; options.height = 48; options.start = 0; options.end = 2; options.fps = fps;
        std::wstring error;
        Check(clock_seq::Prepare(app, options, error), "prepare effect-clock image sequence");
        app->AviOutputWidth() = options.width; app->AviOutputHeight() = options.height;
        app->AviRecordStartFrame() = 0; app->AviRecordEndFrame() = 2; app->AviRecordFps() = fps;
        app->AviIncludeWave() = 0; app->AviStereoOutput() = 0; app->AviCloseAfterExport() = false;
        StartAviRecordWindow(app);
        unsigned steps = 0;
        while (clock_seq::Active(app) && steps++ < 20) {
            app->DeltaTime() = steps % 2 ? .15f : .002f;
            app->state.messageSeen = 1; FrameDriver(app);
            Sleep(steps % 2 ? 150 : 2);
        }
        Check(!clock_seq::Active(app) && effectClockSamples.size() == clock_seq::FrameCount(options),
            "real MME continuous sequence completes without dropped images");
    }
    MH_DisableHook(reinterpret_cast<void*>(&clock_seq::WriteFrame));
    MH_RemoveHook(reinterpret_cast<void*>(&clock_seq::WriteFrame));
}
}
