#include "mikumikustudio/display_motion_snapshot.hpp"

#if !MMD_ORIGINAL_PROFILE
namespace display_sampling_host_checks {
namespace {
std::vector<unsigned char> SourceBytes(const mdl::ModelRecord& m) {
    std::vector<unsigned char> bytes;
    const auto append = [&](const void* source, std::size_t size) {
        if (!size) return;
        const auto* p = static_cast<const unsigned char*>(source);
        bytes.insert(bytes.end(), p, p + size);
    };
    append(&m, sizeof(m));
    append(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys) * sizeof(mdl::DisplayKey));
    append(m.ikChains, m.ikChainCount * sizeof(mdl::IkChain));
    append(m.boneOrderTable, m.boneOrderCount * sizeof(mdl::BoneOrderEntry));
    append(m.boneTable, m.boneCount * sizeof(mdl::BoneRecord));
    append(m.morphs, m.morphCount * sizeof(mdl::MorphRecord));
    for (unsigned slot = 0; slot < 3; ++slot) {
        append(m.displayKeys[slot].ikStates, m.ikChainCount);
        append(m.displayKeys[slot].selectorStates, m.boneOrderCount * sizeof(mdl::BoneReference));
    }
    return bytes;
}
bool Values(const DisplayMotionSnapshot& snapshot, bool edited) {
    std::uint8_t visible = 99;
    DisplaySelectorSample parent;
    return snapshot.SampleVisibility(4.5, visible) == DisplayMotionStatus::Ok && visible == (edited ? 7 : 0) &&
        snapshot.SampleSelector(0, 4.5, parent) == DisplayMotionStatus::Ok &&
        parent.modelIndex == -1 && parent.boneIndex == (edited ? 9 : 7) &&
        parent.windowStart == 4 && parent.windowEnd == (edited ? 8u : 0u);
}
void Workers(const DisplayMotionSnapshot& old, const DisplayMotionSnapshot& newer) {
    bool a = true, b = true;
    std::thread first([&] { for (int i = 0; i < 256; ++i) a = Values(old, false) && a; });
    std::thread second;
    try { second = std::thread([&] { for (int i = 0; i < 256; ++i) b = Values(newer, true) && b; }); }
    catch (...) { first.join(); throw; }
    first.join(); second.join();
    Check(a && b, "display workers preserve two independent revisions and selector windows");
}
}
void Run(MMDApp* app) {
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "display sampling PMX fixture supplied");
    LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0); Check(raw != nullptr, "load real display model");
    auto& m = *mdl::Mdl(raw);
    Check(m.displayKeys && m.boneOrderCount && m.boneOrderTable, "loaded model has global parent selector");
    Check(mdl::DisplayKeyPoolEnsure(m, 3), "prepare display key capacity");
    for (unsigned slot = 0; slot < 3; ++slot) {
        auto& key = m.displayKeys[slot];
        Check((!m.ikChainCount || key.ikStates) && key.selectorStates, "loaded display key owns nested payloads");
        key.frame = slot * 4; key.previous = slot ? slot - 1 : 0; key.next = slot < 2 ? slot + 1 : 0;
        key.visible = slot == 1 ? 0 : 1; key.allocated = 0;
        for (unsigned i = 0; i < m.ikChainCount; ++i) key.ikStates[i] = std::uint8_t((slot + i) % 2);
        for (unsigned i = 0; i < m.boneOrderCount; ++i) key.selectorStates[i] = {-1, 0};
        key.selectorStates[0].boneIndex = slot ? 7 : 0;
    }
    m.maxFrame = (std::max)(m.maxFrame, 8u);
    m.loadComplete = 3; m.displayKeyCursor = 2; m.displayTrackActive = 0;
    for (unsigned i = 0; i < m.ikChainCount; ++i) m.ikChains[i].enabled = 7;
    auto* liveSelectors = static_cast<mdl::BoneOrderEntry*>(m.boneOrderTable);
    liveSelectors[0].windowStart = 123; liveSelectors[0].windowEnd = 456;
    liveSelectors[0].linkedModel = -1; liveSelectors[0].linkedBone = 789;
    app->CurrentFrame() = 3; app->SceneModified() = 0;
    const auto before = SourceBytes(m);
    auto old = DisplayMotionSnapshot::Capture(raw);
    Check(old && old->KeyCount() == 3 && old->IkCount() == m.ikChainCount && old->SelectorCount() == m.boneOrderCount,
        "capture real display source and channel counts");
    std::int32_t mapped = 42;
    Check(old->SelectorBone(0, mapped) && mapped == -1, "global model parent is retained");
    Check(Values(*old, false), "registered parent windows ignore unregistered edited state");
    Check(SourceBytes(m) == before && app->CurrentFrame() == 3 && app->SceneModified() == 0,
        "display capture and samples preserve complete live model and playback state");

    m.displayKeys[1].visible = 7; m.displayKeys[1].selectorStates[0].boneIndex = 9;
    const auto pending = SourceBytes(m);
    auto newer = DisplayMotionSnapshot::Capture(raw);
    Workers(*old, *newer);
    Check(SourceBytes(m) == pending && app->CurrentFrame() == 3 && app->SceneModified() == 0,
        "new display capture remains read-only");
    const auto project = std::filesystem::absolute(L"display-sampling.pmm");
    app->CurrentFrame() = 0;
    Check(SaveSceneCopy(app, project.c_str()), "save actual PMM with display keys");
    ResetAppState(app);
    Check(app->ModelSlot(0) == nullptr, "destroy real source model");
    Workers(*old, *newer);
    wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile();
    Check(app->ModelSlot(0) != nullptr, "reload real display project");
    Workers(*old, *newer);
    ResetAppState(app);
    std::puts("Frozen display host: registered selection windows, read-only capture and source lifetime passed");
}
}
#endif
