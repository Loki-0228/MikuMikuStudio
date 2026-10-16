// Include after edit_history_host_checks.inc. This probe loads real files and
// invokes the real main-window routes; it never calls a registration planner.
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include <array>
#include <climits>
#include <vector>
#include <cstring>

namespace native_registration_host_checks {
constexpr std::array<UINT, 4> commands{525, 524, 527, 526};
constexpr std::array<int, 4> combos{504, 509, 514, 519};

struct Context {
    MMDApp& app;
    mdl::ModelRecord& model;
    plugins::SharedHistory& history;
    MmsSceneApiV1 scene;
    int (*pluginValue)();
    mdl::BoneKey* bones;
    mdl::MorphKey* morphs;
    mdl::DisplayKey* displays;
    std::uint64_t boneIdentity, morphIdentity, displayIdentity;
    std::size_t boneCapacity, morphCapacity, displayCapacity;
    std::array<unsigned char, sizeof(mdl::UndoRing) * 2> rings{};
    std::array<std::uint32_t, 2> undoState{};
    unsigned char undoDirty, redoDirty;
    std::array<unsigned, 4> selected{};
    std::vector<std::pair<unsigned char*, mdl::BoneReference*>> payloads;

    Context(MMDApp& a, plugins::SharedHistory& h, MmsSceneApiV1 s, int (*value)())
        : app(a), model(*mdl::Mdl(a.ModelSlot(0))), history(h), scene(s), pluginValue(value),
          bones(model.boneKeys), morphs(model.morphKeys), displays(model.displayKeys),
          boneIdentity(mdl::BoneKeyPoolIdentity(bones)), morphIdentity(mdl::MorphKeyPoolIdentity(morphs)),
          displayIdentity(mdl::KeyPoolIdentity(displays)), boneCapacity(mdl::BoneKeyCapacity(bones)),
          morphCapacity(mdl::MorphKeyCapacity(morphs)), displayCapacity(mdl::KeyPoolCapacity(displays)),
          undoDirty(model.undoDirty), redoDirty(model.redoDirty) {
        std::memcpy(rings.data(), model.undoRings, rings.size());
        std::memcpy(undoState.data(), model.undoState, sizeof model.undoState);
        for (unsigned lane = 0; lane < 4; ++lane) selected[lane] = unsigned(model.selectedMorphs[lane]);
        for (std::size_t slot = 0; slot < displayCapacity; ++slot)
            payloads.emplace_back(displays[slot].ikStates, displays[slot].selectorStates);
    }
    std::uint64_t Revision() const {
        std::uint64_t result = 0;
        Check(history.Revision(result) == MMS_OK, "registration journal revision is readable"); return result;
    }
    void RevisionMatches() const {
        MmsSceneQueryV1 query{}; query.h = plugins::Header<MmsSceneQueryV1>(); query.flags = MMS_SCENE_CURRENT_TIME;
        MmsSnapshotV1 snapshot{};
        Check(scene.acquire(scene.context, &query, &snapshot) == MMS_OK, "registration public scene snapshot acquired");
        const auto revision = snapshot.sceneRevision; scene.release(scene.context, snapshot.handle);
        Check(revision == Revision(), "actual native and plugin registration share the public scene revision");
    }
    void StableStorageAndPose() const {
        Check(model.boneKeys == bones && mdl::BoneKeyPoolIdentity(bones) == boneIdentity && mdl::BoneKeyCapacity(bones) == boneCapacity &&
              model.morphKeys == morphs && mdl::MorphKeyPoolIdentity(morphs) == morphIdentity && mdl::MorphKeyCapacity(morphs) == morphCapacity &&
              model.displayKeys == displays && mdl::KeyPoolIdentity(displays) == displayIdentity && mdl::KeyPoolCapacity(displays) == displayCapacity,
              "registration keeps sufficiently sized real pools and their ownership identities");
        bool samePayloads = true;
        for (std::size_t slot = 0; slot < displayCapacity; ++slot)
            samePayloads = samePayloads && displays[slot].ikStates == payloads[slot].first &&
                displays[slot].selectorStates == payloads[slot].second;
        Check(samePayloads, "registration keeps every real display slot's owned payload buffers");
        Check(!std::memcmp(rings.data(), model.undoRings, rings.size()) &&
              !std::memcmp(undoState.data(), model.undoState, sizeof model.undoState) &&
              model.undoDirty == undoDirty && model.redoDirty == redoDirty,
              "controlled registration never falls back into either legacy undo ring");
        Check(model.boneTable[0].trans[1] == 4.25f && model.bonePhysicsState[0] == 1 &&
              mdl::Mdl(app.ModelSlot(1))->boneTable[0].trans[0] == 8.5f,
              "registration and cross-frame history preserve unregistered and unrelated model poses");
    }
    void Command(UINT id) {
        SendMessageW(app.MainWindow(), WM_COMMAND, id, 0);
        StableStorageAndPose(); RevisionMatches();
    }
    void Plugin(const wchar_t* label) {
        edit_history_host_checks::Menu(&app, label);
        edit_history_host_checks::FlushObjectUi(&app);
        StableStorageAndPose(); RevisionMatches();
    }
    unsigned MorphKey(unsigned morph, unsigned frame) const {
        unsigned slot = morph;
        for (std::size_t hops = 0; hops < morphCapacity; ++hops) {
            Check(slot < morphCapacity, "registration morph key chain remains bounded");
            if (morphs[slot].frame == frame) return slot;
            if (!morphs[slot].next) break;
            slot = morphs[slot].next;
        }
        return UINT_MAX;
    }
    unsigned DisplayKey(unsigned frame) const {
        unsigned slot = 0;
        for (std::size_t hops = 0; hops < displayCapacity; ++hops) {
            Check(slot < displayCapacity, "registration display key chain remains bounded");
            if (displays[slot].frame == frame) return slot;
            if (!displays[slot].next) break;
            slot = displays[slot].next;
        }
        return UINT_MAX;
    }
    void ClearJournal() {
        Check(!history.UndoCount() && history.Reset() == MMS_OK, "registration subcase clears only its undone history");
        history.Collect(); app.CurrentFrame() = 0; app.SceneModified() = 0;
    }
};

// Capture the author fields the old handlers would modify on a false fallback,
// including every family's selection marks and nested display data.
struct AuthorState {
    std::vector<unsigned char> bytes;
    std::vector<std::pair<std::size_t, const char*>> fields;
    void Field(const char* name) { fields.emplace_back(bytes.size(), name); }
    void Add(const void* data, std::size_t size) {
        if (!size) return;
        const auto* first = static_cast<const unsigned char*>(data); bytes.insert(bytes.end(), first, first + size);
    }
    template<class T> void Add(const T& value) { Add(&value, sizeof value); }
    explicit AuthorState(const Context& c) {
        const auto& m = c.model;
        Field("pool pointers");
        Add(m.boneKeys); Add(m.morphKeys); Add(m.displayKeys);
        Field("bone root keys");
        Add(m.boneKeys, m.boneCount * sizeof(mdl::BoneKey));
        Field("bone selection marks");
        for (std::size_t index = 0; index < c.boneCapacity; ++index) Add(m.boneKeys[index].allocated);
        Field("morph key pool");
        Add(m.morphKeys, c.morphCapacity * sizeof(mdl::MorphKey));
        Field("display key pool");
        Add(m.displayKeys, c.displayCapacity * sizeof(mdl::DisplayKey));
        Field("display nested IK and parent payloads");
        for (std::size_t index = 0; index < c.displayCapacity; ++index) {
            Add(m.displayKeys[index].ikStates, m.ikChainCount);
            Add(m.displayKeys[index].selectorStates, m.boneOrderCount * sizeof(mdl::BoneReference));
        }
        Field("bone author poses and dirty flags");
        for (unsigned index = 0; index < m.boneCount; ++index) {
            Add(m.boneTable[index].trans); Add(m.boneTable[index].rotQuat); Add(m.bonePhysicsState[index]);
        }
        Field("live morph weights");
        for (unsigned index = 0; index < m.morphCount; ++index) Add(m.morphs[index].value);
        Field("live visibility"); Add(m.loadComplete);
        Field("selected morphs"); Add(m.selectedMorphs);
        Field("model maxFrame"); Add(m.maxFrame);
        Field("searchCursor"); Add(m.searchCursor);
        Field("display cursor and track active");
        Add(m.displayKeyCursor); Add(m.displayTrackActive);
        Field("bone key cursors");
        Add(m.boneKeyCursors, m.boneCount * sizeof(std::uint32_t));
        Field("morph key cursors");
        Add(m.morphKeyCursors, m.morphCount * sizeof(std::uint32_t));
        Field("bone and morph track active");
        Add(m.boneTrackActive, m.boneCount); Add(m.morphTrackActive, m.morphCount);
        Field("current parent selectors");
        Add(m.boneOrderTable, m.boneOrderCount * sizeof(mdl::BoneOrderEntry));
        Field("current IK enabled");
        for (unsigned index = 0; index < m.ikChainCount; ++index) Add(m.ikChains[index].enabled);
        Field("current frame"); Add(c.app.CurrentFrame());
        Field("last registered frame"); Add(c.app.LastRegisteredFrame());
        Field("scene dirty"); Add(c.app.SceneModified());
        Field("camera mode"); Add(c.app.CameraMode());
        Field("selected model"); Add(c.app.SelectedModelSlot());
        Field("undo count"); Add(c.history.UndoCount());
        Field("redo count"); Add(c.history.RedoCount());
        Field("journal revision"); Add(c.Revision());
    }
    bool Matches(const Context& c) const {
        const AuthorState after(c);
        if (bytes == after.bytes) return true;
        if (bytes.size() != after.bytes.size()) {
            std::printf("DIFF author snapshot bytes before=%zu after=%zu\n", bytes.size(), after.bytes.size());
        } else {
            unsigned printed = 0;
            for (std::size_t index = 0; index < fields.size() && printed < 12; ++index) {
                const auto begin = fields[index].first;
                const auto end = index + 1 < fields.size() ? fields[index + 1].first : bytes.size();
                for (auto offset = begin; offset < end; ++offset) if (bytes[offset] != after.bytes[offset]) {
                    std::printf("DIFF author field=%s byte=%zu before=%u after=%u\n", fields[index].second,
                        offset - begin, unsigned(bytes[offset]), unsigned(after.bytes[offset]));
                    ++printed; break;
                }
            }
        }
        std::fflush(stdout); return false;
    }
};

void MorphLanes(Context& c) {
    auto& m = c.model;
    for (unsigned lane = 0; lane < commands.size(); ++lane) {
        std::printf("RUN real morph registration lane=%u command=%u\n", lane, commands[lane]); std::fflush(stdout);
        const auto morph = c.selected[lane];
        const float first = .25f + .125f * lane, second = .875f;
        const int pluginBefore = c.pluginValue();
        c.app.CurrentFrame() = 10; m.morphs[morph].value = first;
        const auto initialRevision = c.Revision(); c.app.SceneModified() = 0;
        c.Command(commands[lane]);
        const auto slot = c.MorphKey(morph, 10);
        Check(slot != UINT_MAX && slot >= m.morphCount && m.morphKeys[slot].value == first && m.morphKeys[slot].allocated &&
              c.history.UndoCount() == 1 && c.history.RedoCount() == 0 && c.Revision() > initialRevision && c.app.SceneModified(),
              "real lane button inserts its selected morph into one common-history node");
        Check(m.morphs[morph].value == first && c.app.CurrentFrame() == 10 && m.maxFrame >= 10 && c.app.LastRegisteredFrame() >= 10,
              "first morph registration preserves current author weight and updates timeline metadata");
        for (unsigned other = 0; other < 4; ++other) if (other != lane)
            Check(m.morphs[c.selected[other]].value == .1875f, "registering one lane preserves unrelated live morph weights");
        c.Plugin(L"Edit probe once");
        Check(c.pluginValue() == pluginBefore + 1 && c.history.UndoCount() == 2, "real plugin edit interleaves between morph insert and overwrite");
        m.morphs[morph].value = second; c.Command(commands[lane]);
        Check(c.MorphKey(morph, 10) == slot && m.morphKeys[slot].value == second &&
              m.morphs[morph].value == second && c.history.UndoCount() == 3,
              "real lane overwrite reuses its key slot and adds one journal node without seeking");
        c.app.CurrentFrame() = 5;
        c.Plugin(L"Edit probe undo");
        Check(m.morphKeys[slot].value == first && m.morphs[morph].value == first * .5f && c.app.CurrentFrame() == 5 &&
              c.history.UndoCount() == 2 && c.history.RedoCount() == 1,
              "plugin undo restores morph author key and evaluates it at the changed playhead");
        c.Command(400);
        Check(c.pluginValue() == pluginBefore && c.history.UndoCount() == 1, "native undo reverses only the interleaved plugin edit");
        c.Command(400);
        Check(c.MorphKey(morph, 10) == UINT_MAX && m.morphs[morph].value == 0 &&
              !c.history.UndoCount() && c.history.RedoCount() == 3, "native undo removes the inserted key and preserves three redo steps");
        c.app.CurrentFrame() = 0; c.app.SceneModified() = 0;
        m.boneKeys[0].allocated = 1; m.displayKeys[0].allocated = 1;
        const AuthorState same(c); c.Command(commands[lane]);
        Check(same.Matches(c), "same-value root registration preserves dirty metadata all-family marks revision and redo");
        c.app.CurrentFrame() = 20; c.Plugin(L"Edit probe redo");
        Check(c.MorphKey(morph, 10) == slot && m.morphs[morph].value == first && c.app.CurrentFrame() == 20,
              "plugin redo restores insertion using the current frame");
        Check(!m.boneKeys[0].allocated && !m.displayKeys[0].allocated && m.morphKeys[slot].allocated,
              "morph redo clears bone and display marks selected after undo and selects its registered key");
        c.Command(401); c.Plugin(L"Edit probe redo");
        Check(c.pluginValue() == pluginBefore + 1 && m.morphs[morph].value == second &&
              m.morphKeys[slot].value == second && c.history.UndoCount() == 3,
              "native and plugin redo restore the mixed morph edit sequence");
        c.Plugin(L"Edit probe undo"); c.Command(400); c.Plugin(L"Edit probe undo");
        Check(c.pluginValue() == pluginBefore && !m.morphKeys[morph].next, "mixed undo returns the lane to its original root track");
        c.ClearJournal(); m.morphs[morph].value = .1875f;
    }
}

void DisplayRegistration(Context& c) {
    std::puts("RUN real display IK external-parent registration 438"); std::fflush(stdout);
    auto& m = c.model; auto* order = static_cast<mdl::BoneOrderEntry*>(m.boneOrderTable);
    const auto parentSlot = unsigned(m.boneTable[0].slotIndex);
    Check(parentSlot && parentSlot < m.boneOrderCount, "real movable bone has an external-parent selector");
    const int pluginBefore = c.pluginValue();
    c.app.CurrentFrame() = 10; m.loadComplete = 0; m.ikChains[0].enabled = 0;
    order[parentSlot].linkedModel = 1; order[parentSlot].linkedBone = 0;
    const std::vector<mdl::BoneOrderEntry> authored(order, order + m.boneOrderCount);
    c.app.SceneModified() = 0; const auto revision = c.Revision(); c.Command(438);
    const auto slot = c.DisplayKey(10);
    Check(slot != UINT_MAX && slot != 0 && !m.displayKeys[slot].visible && !m.displayKeys[slot].ikStates[0] &&
          m.displayKeys[slot].selectorStates[parentSlot].modelIndex == 1 &&
          m.displayKeys[slot].selectorStates[parentSlot].boneIndex == 0 && m.displayKeys[slot].allocated &&
          c.history.UndoCount() == 1 && c.app.SceneModified() && c.Revision() > revision,
          "actual 438 records visibility IK and external parent together in one node");
    Check(!m.loadComplete && !m.ikChains[0].enabled && !std::memcmp(authored.data(), order, authored.size() * sizeof(authored[0])) &&
          c.app.CurrentFrame() == 10, "first display registration preserves authored state and selector windows without a seek");
    c.Plugin(L"Edit probe once");
    m.loadComplete = 1; m.ikChains[0].enabled = 1; order[parentSlot].linkedBone = 1;
    c.Command(438);
    Check(c.DisplayKey(10) == slot && m.displayKeys[slot].visible && m.displayKeys[slot].ikStates[0] &&
          m.displayKeys[slot].selectorStates[parentSlot].boneIndex == 1 && c.history.UndoCount() == 3,
          "438 overwrite reuses owned arrays and joins the plugin/native sequence");
    c.app.CurrentFrame() = 5; c.Plugin(L"Edit probe undo");
    Check(!m.displayKeys[slot].visible && !m.displayKeys[slot].ikStates[0] &&
          m.displayKeys[slot].selectorStates[parentSlot].boneIndex == 0 && m.loadComplete &&
          m.ikChains[0].enabled && order[parentSlot].linkedModel == -1 && c.app.CurrentFrame() == 5,
          "display undo restores author payload but samples root visibility IK and parent before the key");
    c.Command(400); c.Command(400);
    Check(c.pluginValue() == pluginBefore && !m.displayKeys[0].next && !c.history.UndoCount() && c.history.RedoCount() == 3,
          "native undo removes the plugin step and the display insertion independently");
    c.app.CurrentFrame() = 0; c.app.SceneModified() = 0;
    m.boneKeys[0].allocated = 1; m.morphKeys[c.selected[0]].allocated = 1;
    const AuthorState same(c); c.Command(438);
    Check(same.Matches(c), "same-value display registration keeps marks dirty metadata revision and redo unchanged");
    c.app.CurrentFrame() = 15; c.Plugin(L"Edit probe redo");
    Check(!m.loadComplete && !m.ikChains[0].enabled && order[parentSlot].linkedModel == 1 &&
          order[parentSlot].linkedBone == 0 && order[parentSlot].windowStart == 10 && c.app.CurrentFrame() == 15,
          "plugin redo derives the inserted display IK and external-parent window at the current frame");
    c.Command(401); c.Plugin(L"Edit probe redo");
    Check(m.loadComplete && m.ikChains[0].enabled && order[parentSlot].linkedModel == 1 &&
          order[parentSlot].linkedBone == 1 && c.pluginValue() == pluginBefore + 1 && c.history.UndoCount() == 3,
          "mixed redo restores overwritten visibility IK and external parent");
    c.app.CameraMode() = 1; c.app.CurrentFrame() = 20;
    c.Plugin(L"Edit probe undo"); c.Plugin(L"Edit probe redo");
    Check(c.app.CameraMode() && c.app.CurrentFrame() == 20 && order[parentSlot].linkedBone == 1,
          "display history stays independent of current model mode and playhead");
    c.app.CameraMode() = 0;
    c.Plugin(L"Edit probe undo"); c.Command(400); c.Plugin(L"Edit probe undo");
    c.ClearJournal();
}

void Rejections(Context& c) {
    std::puts("RUN actual registration busy invalid selection and no-fallback checks"); std::fflush(stdout);
    auto& m = c.model;
    c.app.CurrentFrame() = 30; c.app.SceneModified() = 0;
    for (unsigned lane = 0; lane < 4; ++lane) m.morphs[c.selected[lane]].value = .875f;
    m.loadComplete = 0; m.ikChains[0].enabled = 0; m.boneKeys[0].allocated = 1;
    const auto refused = [&](const char* scenario) {
        const AuthorState before(c);
        for (const auto command : {525u, 524u, 527u, 526u, 438u}) {
            std::printf("RUN rejected registration scenario=%s command=%u\n", scenario, command); std::fflush(stdout);
            c.Command(command);
            Check(before.Matches(c), "blocked actual registration buttons consume the request without old writes or history changes");
        }
    };
    c.app.PlaybackActive() = 1; refused("playback"); c.app.PlaybackActive() = 0;
    c.app.CameraMode() = 1; refused("camera"); c.app.CameraMode() = 0;
    c.Plugin(L"Edit probe prepared");
    const auto pending = reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(GetModuleHandleW(L"manager_edit_probe.dll"), "GetProbePending"));
    Check(pending && pending(), "real plugin retains its prepared transaction during native registration rejection");
    refused("plugin prepared"); c.Plugin(L"Edit probe cancel");
    for (unsigned lane = 0; lane < 4; ++lane) {
        for (int invalid : {-1, int(m.morphCount)}) {
            m.selectedMorphs[lane] = invalid;
            const AuthorState before(c); c.Command(commands[lane]);
            Check(before.Matches(c), "negative or out-of-range selected morph does not reach the old registrar");
        }
        m.selectedMorphs[lane] = int(c.selected[lane]);
    }
    if (m.physicsMode != 2) {
        m.selectedMorphs[0] = 0;
        const AuthorState before(c); c.Command(commands[0]);
        Check(before.Matches(c), "PMD base morph zero is refused without clearing selection or dirtying the project");
        m.selectedMorphs[0] = int(c.selected[0]);
    }
    c.app.CurrentFrame() = -1; refused("negative frame"); c.app.CurrentFrame() = 0;
    Check(!c.history.UndoCount() && !c.history.RedoCount(), "refused registrations leave the common journal empty");
    c.app.SceneModified() = 0;
}

void Run(MMDApp* app) {
    using namespace plugins;
    const auto initial = GetModuleHandleW(L"manager_edit_probe.dll");
    const auto initialFlags = reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(initial, "GetProbeFlags"));
    Check(initial && initialFlags && (initialFlags() & 2) && !(initialFlags() & 1), "ordinary registration host keeps mms.edit unavailable");
    wchar_t path[256]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 256), "registration model path supplied");
    LoadModelFile(app, path, false); LoadModelFile(app, path, false);
    Check(app->ModelSlot(0) && app->ModelSlot(1), "two real models provide a valid external-parent target");
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    Check(model.boneCount == 4 && model.ikChainCount == 1 && model.boneOrderCount >= 3 &&
          model.bonePhysicsState && model.morphCount >= 4, "file fixture provides four morph panels real IK and parent selectors");
    app->SetSelectedModelSlot(0); app->CameraMode() = 0;
    SendDlgItemMessageW(app->MainWindow(), 436, CB_SETCURSEL, model.comboSelIndex, 0);
    PostLoadInit(app->ModelSlot(0));
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto control = GetDlgItem(app->MainWindow(), combos[lane]);
        const auto row = SendMessageW(control, CB_GETCURSEL, 0, 0);
        const int morph = model.selectedMorphs[lane];
        Check(control && row >= 0 && morph >= 0 && unsigned(morph) < model.morphCount &&
              SendMessageW(control, CB_GETITEMDATA, row, 0) == morph && unsigned(model.morphs[morph].panel) == lane + 1,
              "real post-load UI selects the correct morph in every registration lane");
        model.morphs[morph].value = .1875f;
    }
    model.boneTable[0].trans[1] = 4.25f; model.bonePhysicsState[0] = 1;
    mdl::Mdl(app->ModelSlot(1))->boneTable[0].trans[0] = 8.5f;
    mdl::Mdl(app->ModelSlot(1))->bonePhysicsState[0] = 1;
    Shutdown(); Check(InitializeHistoryTestRuntime(app), "explicit registration host attaches one real plugin/native journal");
    auto* history = NativeHistory(app);
    Check(history && BoundNativePoseHistory(app) == history, "registration runtime exposes only its explicit bound history");
    const auto library = GetModuleHandleW(L"manager_edit_probe.dll");
    const auto value = reinterpret_cast<int(*)()>(GetProcAddress(library, "GetProbeValue"));
    const auto flags = reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(library, "GetProbeFlags"));
    Check(value && flags && (flags() & 1) && !(flags() & 2), "real registration plugin negotiates controlled edit service");
    auto scene = CreateSceneStore(app); SceneClient client;
    Context context(*app, *history, scene->Api(client), value);
    MorphLanes(context); DisplayRegistration(context); Rejections(context);
    std::puts("PASS actual four-lane morph and display/IK/parent registration with native/plugin history");
}
}
