// Exercise the retained unbound command, including selected non-head neighbours.
#include "mikudancestudio/animation_curve_host.hpp"
namespace {
void GlobalZeroDeleteChecks() {
    unsigned scenarios = 0;
    for (int family = 0; family < 4; ++family) for (unsigned mask : {1u, 3u, 5u, 7u})
    for (int frame : {0, 30, 45, 60, 75}) {
        auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
        auto renderer = std::make_unique<D3DRenderer>(); std::memset(renderer.get(), 0, sizeof(*renderer));
        app->Renderer() = renderer.get(); app->CameraMode() = 1; app->CurrentFrame() = frame;
        std::vector<mdl::CameraKey> camera(mdl::kTimelineKeyCapacity);
        std::vector<mdl::LightKey> light(mdl::kTimelineKeyCapacity);
        std::vector<mdl::SelfShadowKey> shadow(mdl::kTimelineKeyCapacity);
        std::vector<mdl::GravityKey> gravity(mdl::kTimelineKeyCapacity);
        app->CameraKeys() = camera.data(); app->LightKeys() = light.data();
        app->ShadowKeys() = shadow.data(); app->GravityKeys() = gravity.data();
        auto prepare = [&](auto& keys, int kind) {
            for (unsigned i = 0; i < 3; ++i) {
                keys[i].frame = i * 30; keys[i].previous = i ? i - 1 : 0;
                keys[i].next = i < 2 ? i + 1 : 0;
                keys[i].selected = kind == family && (mask & (1u << i)) ? 1 : 0;
            }
        };
        prepare(camera, 0); prepare(light, 1); prepare(shadow, 2); prepare(gravity, 3);
        for (unsigned i = 0; i < 3; ++i) {
            camera[i].parentModel = -1; camera[i].fov = 45; camera[i].distance = -45.f;
            light[i].color[0] = .25f + .1f * float(i); light[i].direction[1] = -1.f;
            shadow[i].mode = 1; shadow[i].distance = .01f + .001f * float(i);
            gravity[i].acceleration = 9.8f + float(i); gravity[i].direction[1] = -1.f;
        }
        const auto oldCamera = camera; const auto oldLight = light;
        const auto oldShadow = shadow; const auto oldGravity = gravity;
        auto* previous = g_Block; g_Block = app.get();
        DeleteMarkedKeyframes(app.get()); g_Block = previous;
        const auto verify = [&](const auto& keys, const auto& before, int kind) {
            using Key = typename std::decay_t<decltype(keys)>::value_type;
            if (kind != family) {
                Check(!std::memcmp(keys.data(), before.data(), keys.size() * sizeof(Key)),
                    "deleting another global family leaves this entire key pool unchanged");
                return;
            }
            const unsigned first = !(mask & 2u) ? 1u : !(mask & 4u) ? 2u : 0u;
            Check(keys[0].frame == 0 && !keys[0].selected && keys[0].next == first,
                "resetting a selected global zero key preserves the surviving successor chain");
            for (unsigned i = 1; i < 3; ++i) {
                if (mask & (1u << i)) {
                    Check(!keys[i].frame && !keys[i].previous && !keys[i].next && !keys[i].selected,
                        "selected nonzero global key is unlinked and released");
                } else {
                    auto expected = before[i];
                    expected.previous = i == 2 && !(mask & 2u) ? 1u : 0u;
                    expected.next = i == 1 && !(mask & 4u) ? 2u : 0u;
                    Check(!std::memcmp(&keys[i], &expected, sizeof(Key)),
                        "unselected global successor keeps every payload byte and correct links");
                }
            }
        };
        verify(camera, oldCamera, 0); verify(light, oldLight, 1);
        verify(shadow, oldShadow, 2); verify(gravity, oldGravity, 3);
        if (mask == 1) {
            const unsigned left = frame >= 60 ? 2u : frame >= 30 ? 1u : 0u;
            if (family == 1) {
                const float expected = frame == 45 ?
                    float(double(light[2].color[0] - light[1].color[0]) * .5 + double(light[1].color[0])) :
                    light[left].color[0];
                Check(std::fabs(app->LightColor()[0] - expected) < 1.e-7f,
                    "real light sampler still reaches surviving keys at exact, intermediate and later frames");
            }
            if (family == 2) Check(app->ShadowDistance() == shadow[left].distance,
                "real shadow sampler still reaches surviving keys and preserves its held interval value");
            if (family == 3) {
                const float expected = frame == 45 ?
                    .5f * (gravity[2].acceleration - gravity[1].acceleration) + gravity[1].acceleration :
                    gravity[left].acceleration;
                Check(std::fabs(app->GravityMagnitude() - expected) < 1.e-6f,
                    "real gravity sampler still reaches surviving keys without a physics device");
            }
        }
        curve_host::Reset(app.get());
        ++scenarios;
    }
    std::printf("PASS global zero deletion: %u family/selection scenarios\n", scenarios);
}
}
