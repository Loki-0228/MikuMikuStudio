#include "mikumikustudio/morph_curve.hpp"

#if !MMD_ORIGINAL_PROFILE
namespace morph_sampling_host_checks {
namespace {
std::vector<unsigned char> CopyBytes(const void* data, std::size_t size) {
    std::vector<unsigned char> bytes(size);
    if (size) std::memcpy(bytes.data(), data, size);
    return bytes;
}

// Compare both the native data and every authoring layer. Comparing only the
// effective curve would miss a capture that silently refreshed live caches.
struct Unchanged {
    MMDApp* app;
    unsigned char* raw;
    std::vector<unsigned char> model, morphs, keys, cursors, active;
    morph_curve::AuthoredLayers layers;
    int frame, modified;
    explicit Unchanged(MMDApp* owner, unsigned char* source)
        : app(owner), raw(source), frame(owner->CurrentFrame()), modified(owner->SceneModified()) {
        const auto& m = *mdl::Mdl(raw);
        model = CopyBytes(&m, sizeof(m));
        morphs = CopyBytes(m.morphs, m.morphCount * sizeof(mdl::MorphRecord));
        keys = CopyBytes(m.morphKeys, mdl::MorphKeyCapacity(m) * sizeof(mdl::MorphKey));
        if (m.morphKeyCursors) cursors = CopyBytes(m.morphKeyCursors, m.morphCount * sizeof(std::uint32_t));
        if (m.morphTrackActive) active = CopyBytes(m.morphTrackActive, m.morphCount);
        std::string error;
        Check(morph_curve::CaptureLayers(raw, layers, error), "capture real morph authoring proof");
    }
    void Verify() const {
        const auto& m = *mdl::Mdl(raw);
        Check(app->CurrentFrame() == frame && app->SceneModified() == modified,
            "morph capture and sampling preserve playback frame and modified flag");
        Check(!std::memcmp(model.data(), &m, model.size()), "morph capture preserves complete model record");
        Check(!std::memcmp(morphs.data(), m.morphs, morphs.size()), "morph capture preserves edited morph records");
        Check(!std::memcmp(keys.data(), m.morphKeys, keys.size()), "morph capture preserves complete native key pool");
        Check(cursors.empty() || !std::memcmp(cursors.data(), m.morphKeyCursors, cursors.size()),
            "morph capture preserves live cursors");
        Check(active.empty() || !std::memcmp(active.data(), m.morphTrackActive, active.size()),
            "morph capture preserves active track flags");
        Check(morph_curve::LayersUnchanged(raw, layers), "morph capture preserves pristine effective mirror and observed layers");
    }
};

bool SampleEquals(const morph_curve::SamplingState& source, int morph, double frame, float expected) {
    float value = -123.f;
    return source.Sample(morph, frame, value) && std::isfinite(value) && std::abs(value - expected) < 1e-4f;
}

void ReadOnWorkers(const morph_curve::SamplingState& first, const morph_curve::SamplingState& newer, int morph) {
    bool oldOk = true, newOk = true;
    std::thread oldReader([&] {
        for (int i = 0; i < 256; ++i)
            oldOk = SampleEquals(first, morph, i % 2 ? 2.0 : 4.0, i % 2 ? 4.f : 0.f) && oldOk;
    });
    // No host/UI calls or assertions on the worker; only immutable data reads.
    std::thread newReader;
    try {
        newReader = std::thread([&] {
            for (int i = 0; i < 256; ++i)
                newOk = SampleEquals(newer, morph, i % 2 ? 4.0 : 2.0, i % 2 ? 12.f : 6.f) && newOk;
        });
    } catch (...) { oldReader.join(); throw; }
    oldReader.join(); newReader.join();
    Check(oldOk && newOk, "concurrent frozen morph reads preserve distinct source revisions");
}
}

void Run(MMDApp* app) {
    wchar_t modelPath[32768]{};
    const auto pathLength = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", modelPath, 32768);
    Check(pathLength > 0 && pathLength < 32768, "morph sampling PMX fixture supplied");
    LoadModelFile(app, modelPath, false);
    auto* raw = app->ModelSlot(0);
    Check(raw != nullptr, "load real morph sampling model");
    auto& model = *mdl::Mdl(raw);
    Check(model.morphCount && model.morphs && model.morphKeys, "real fixture contains morph records and keys");
    const auto available = morph_curve::Enumerate(raw);
    int selected = -1;
    for (const auto& morph : available) if (morph.panel >= 1 && morph.panel <= 4) { selected = morph.index; break; }
    Check(selected >= 0, "real fixture contains an editable morph; missing morph is not skipped");
    animation_curve::NamedTrack track;
    track.id = morph_curve::StableId(selected, model.morphs[selected].name);
    animation_curve::Key firstKey, lastKey;
    lastKey.frame = 4;
    firstKey.outSlope = 4; lastKey.inSlope = -4;
    firstKey.tangent = lastKey.tangent = animation_curve::Tangent::Free;
    track.track.keys = {firstKey, lastKey};
    std::string error;
    Check(morph_curve::Write(raw, {track}, error), "write real registered free morph curve");
    // An unrelated unregistered pose must neither supply samples nor be overwritten.
    model.morphs[selected].value = .375f;
    app->CurrentFrame() = 7; app->SceneModified() = 0;
    const Unchanged original(app, raw);
    auto first = morph_curve::SamplingState::Capture(raw);
    Check(first && first->MorphCount() == model.morphCount && first->RetainedBytes() > 0,
        "capture owned real morph sampling state");
    Check(SampleEquals(*first, selected, 2, 4), "real authored morph overshoots beyond unit weight");
    Check(SampleEquals(*first, selected, 4, 0), "registered endpoint ignores unregistered live pose");
    original.Verify();

    const auto endpoint = model.morphKeys[selected].next;
    Check(endpoint && endpoint < mdl::MorphKeyCapacity(model) && model.morphKeys[endpoint].frame == 4,
        "real morph native endpoint exists");
    model.morphKeys[endpoint].value = 12.f; // Deliberately do not call Read, Sync or Evaluate.
    const Unchanged pending(app, raw);
    auto newer = morph_curve::SamplingState::Capture(raw);
    Check(newer && SampleEquals(*newer, selected, 4, 12) && SampleEquals(*newer, selected, 2, 6),
        "new capture privately reconciles unsynchronized native morph edit");
    Check(SampleEquals(*first, selected, 2, 4) && SampleEquals(*first, selected, 4, 0),
        "native edit does not change previous frozen morph state");
    ReadOnWorkers(*first, *newer, selected);
    pending.Verify();

    const auto directory = std::filesystem::absolute(L"morph-sampling-host-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(std::filesystem::create_directory(directory), "create fresh isolated morph project directory");
    const auto project = directory / L"morph-sampling.pmm";
    app->CurrentFrame() = 0;
    Check(SaveSceneCopy(app, project.c_str()), "save actual PMM for morph source replacement");
    ResetAppState(app);
    Check(app->ModelSlot(0) == nullptr, "actual reset destroys source model");
    ReadOnWorkers(*first, *newer, selected);
    wcscpy_s(app->state.envFileName, project.c_str());
    LoadSceneFile();
    Check(app->ModelSlot(0) != nullptr, "reload actual saved morph project");
    ReadOnWorkers(*first, *newer, selected);
    ResetAppState(app);
    std::puts("Frozen morph host: real authoring, private reconciliation, unchanged editing and source lifetime passed");
}
}
#endif
