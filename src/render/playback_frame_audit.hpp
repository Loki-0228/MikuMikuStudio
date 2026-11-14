#pragma once

#include "../app/playback_physics_samples.hpp"
#include <cstdio>
#include <vector>

namespace mikumikustudio::frame_rate::audit {
// Opt-in measurement. Keep frame records in memory and write only after playback.
// QPC timestamps allow comparison with an independent PresentMon trace.
struct Row {
    long long qpc = 0;
    unsigned long long presented = 0, step = 0, pose = 14695981039346656037ull;
    double seconds = 0, completed = 0, hudEpoch = 0;
    float limit = 0;
    int hud = 0, bones = 0, finite = 1, ready = 0, covered = 0, interpolate = 0;
    double costUs = 0;
};
struct Recorder {
    wchar_t path[32768]{};
    LARGE_INTEGER frequency{};
    std::vector<Row> rows;
    bool written = false;
    Recorder() {
        const auto size = GetEnvironmentVariableW(L"MIKUMIKUSTUDIO_PLAYBACK_AUDIT", path, 32768);
        if (!size || size >= 32768 || !QueryPerformanceFrequency(&frequency)) { path[0] = 0; return; }
        rows.reserve(120000);
    }
    void Flush() {
        if (!path[0] || rows.empty() || written) return;
        written = true;
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"wb") || !file) return;
        std::fprintf(file, "qpc,qpc_frequency,presented,seconds,physics_step,physics_completed,pose_hash,bones,finite,ready,covered,interpolate,limit,hud_fps,hud_epoch,audit_us\n");
        for (const auto& r : rows)
            std::fprintf(file, "%lld,%lld,%llu,%.9f,%llu,%.9f,%llu,%d,%d,%d,%d,%d,%.3f,%d,%.9f,%.3f\n",
                r.qpc, frequency.QuadPart, r.presented, r.seconds, r.step, r.completed,
                r.pose, r.bones, r.finite, r.ready, r.covered, r.interpolate, r.limit,
                r.hud, r.hudEpoch, r.costUs);
        std::fclose(file);
    }
    ~Recorder() { Flush(); }
};
inline Recorder& State() { static Recorder recorder; return recorder; }
inline void Idle(MMDApp* app) {
    if (!app->PlaybackActive()) State().Flush();
}
inline void Presented(MMDApp* app) {
    auto& recorder = State();
    if (!recorder.path[0] || recorder.written || recorder.rows.size() == recorder.rows.capacity()) return;
    Row row;
    LARGE_INTEGER start{}, end{};
    QueryPerformanceCounter(&start); row.qpc = start.QuadPart;
    const auto& clock = app->PlaybackPhysicsClock();
    row.presented = app->frameRate.presentedFrames;
    row.seconds = app->frameRate.playbackSample;
    row.step = clock.step;
    row.completed = clock.Completed(playback_timing::PhysicsRate(app->PlaybackTiming()));
    row.ready = clock.visualSampleReady;
    row.covered = clock.samples && clock.samples->Covers(row.seconds);
    row.interpolate = app->PlaybackTiming().interpolate;
    row.limit = app->EffectiveDisplayFps(); row.hud = app->FramesPerSecond();
    row.hudEpoch = app->frameRate.epoch;
    for (int slot = 0; slot < kModelSlotCount; ++slot) if (const auto* raw = app->ModelSlot(slot)) {
        const auto& model = *mdl::Mdl(raw);
        if (!model.boneTable) continue;
        row.bones += model.boneCount;
        for (unsigned b = 0; b < model.boneCount; ++b)
            for (float value : model.boneTable[b].matInit) {
                row.finite &= std::isfinite(value);
                std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
                row.pose = (row.pose ^ bits) * 1099511628211ull;
            }
    }
    QueryPerformanceCounter(&end);
    row.costUs = double(end.QuadPart - start.QuadPart) * 1e6 / recorder.frequency.QuadPart;
    recorder.rows.push_back(row);
}
}
