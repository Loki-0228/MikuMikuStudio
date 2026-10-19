#pragma once

// Render-thread scopes only. Nested measurements are inclusive; never add
// parent and child totals. No clocks or storage in ordinary builds.
#ifdef MIKUMIKUSTUDIO_PERF
#include <chrono>
#include <cstdio>
namespace mikumikustudio::perf {
enum Phase { Frame, Scene, VertexUpdate, Morph, Skin, Sort, Materials,
             Physics, Present, SortLock, SortCpu, Pose, PhysicsSync, PhysicsReadback, PhysicsStep, BoneMorph,
             Accessories, Edges, ShadowMap, GroundShadow, ResourceGuard,
             Clear, Composition, Overlays, EndScene, ValidationRelease, ScreenCapture, BeginScene, EffectApi, ShadowBounds, ShadowBoundsLock, Count };
inline double milliseconds[Count]{};
inline unsigned calls[Count]{};
class Scope {
    Phase phase;
    std::chrono::steady_clock::time_point start;
public:
    explicit Scope(Phase p) : phase(p), start(std::chrono::steady_clock::now()) {}
    ~Scope() {
        milliseconds[phase] += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        ++calls[phase];
    }
};
inline void Reset() {
    for (int i=0; i<Count; ++i) { milliseconds[i]=0; calls[i]=0; }
}
inline void Report(unsigned frames) {
    const char* names[] = {"frame", "scene", "vertex_update", "morph", "skin",
                           "transparent_sort", "materials", "physics", "present", "sort_lock", "sort_cpu", "pose", "physics_sync", "physics_readback", "physics_step", "bone_morph",
                           "accessories", "edges", "shadow_map", "ground_shadow", "resource_guard",
                           "clear", "composition", "overlays", "end_scene", "validation_release", "screen_capture", "begin_scene", "effect_api", "shadow_bounds", "shadow_bounds_lock"};
    for (int i=0; i<Count; ++i)
        std::printf("phase=%s inclusive_ms_per_frame=%.4f calls=%u\n",
                    names[i], milliseconds[i]/frames, calls[i]);
}
}
#define MMD_PERF_SCOPE(phase) mikumikustudio::perf::Scope mmdPerfScope(mikumikustudio::perf::phase)
#else
#define MMD_PERF_SCOPE(phase) ((void)0)
#endif
