// Include after edit_history_host_checks.inc. This probe uses real command
// dispatch and the loaded edit DLL, never a camera interpolation planner.
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include <array>
#include <cstring>
#include <vector>

namespace camera_interpolation_host_checks {
struct Bytes {
    std::vector<unsigned char> data;
    void Add(const void* p, std::size_t n) {
        if (n) { const auto* begin = static_cast<const unsigned char*>(p); data.insert(data.end(), begin, begin + n); }
    }
    template<class T> void Add(const T& value) { Add(&value, sizeof value); }
    template<class T> void Pool(T* pool) {
        Add(pool); const auto capacity = mdl::KeyPoolCapacity(pool); Add(capacity);
        Add(mdl::KeyPoolIdentity(pool)); Add(pool, capacity * sizeof(T));
    }
};
using CameraKeys = std::vector<mdl::CameraKey>;
using Curves = std::array<std::array<unsigned char, 4>, 6>;

CameraKeys Keys(MMDApp& app) {
    auto* pool = app.CameraKeys(); CameraKeys keys(pool, pool + mdl::KeyPoolCapacity(pool));
    for (auto& key : keys) key.selected = 0; // Marks belong to the current view, including during history replay.
    return keys;
}
void SameKeys(MMDApp& app, const CameraKeys& expected, const char* message) {
    const auto current = Keys(app);
    Check(current.size() == expected.size() && !std::memcmp(current.data(), expected.data(), current.size() * sizeof(mdl::CameraKey)), message);
}
void Mark(MMDApp& app, bool alternate = false) {
    auto* keys = app.CameraKeys();
    for (std::size_t i = 0; i < mdl::KeyPoolCapacity(keys); ++i) keys[i].selected = 0;
    keys[0].selected = 2; keys[alternate ? 2 : 1].selected = 3; keys[3].selected = 4;
}
Curves Presets(MMDApp& app) {
    Curves out{};
    for (unsigned lane = 0; lane < 6; ++lane)
        out[lane] = {app.state.lightA[lane], app.state.lightB[lane], app.state.lightC[lane], app.state.lightD[lane]};
    return out;
}
void Presets(MMDApp& app, const Curves& values) {
    for (unsigned lane = 0; lane < 6; ++lane) {
        app.state.lightA[lane] = values[lane][0]; app.state.lightB[lane] = values[lane][1];
        app.state.lightC[lane] = values[lane][2]; app.state.lightD[lane] = values[lane][3];
    }
}
CameraKeys Changed(CameraKeys keys, int channel, const Curves& values) {
    for (unsigned slot : {0u, 1u, 3u})
        for (unsigned lane = 0; lane < 6; ++lane)
            if (channel == 6 || channel == int(lane))
                for (unsigned point = 0; point < 4; ++point) keys[slot].interpolation[point][lane] = values[lane][point];
    return keys;
}

struct Context {
    MMDApp& app; plugins::SharedHistory& history; MmsSceneApiV1 scene;
    int (*pluginValue)(); std::uint64_t (*pending)(); HWND combo;
    mdl::CameraKey* pool; std::size_t capacity; std::uint64_t identity;
    Context(MMDApp& a, plugins::SharedHistory& h, MmsSceneApiV1 s, int (*value)(), std::uint64_t (*p)())
        : app(a), history(h), scene(s), pluginValue(value), pending(p),
          combo(GetDlgItem(a.MainWindow(), panel::kInterpCurveCombo)), pool(a.CameraKeys()),
          capacity(mdl::KeyPoolCapacity(pool)), identity(mdl::KeyPoolIdentity(pool)) {
        Check(combo && SendMessageW(combo, CB_GETCOUNT, 0, 0) == 7,
              "real camera interpolation combo exposes six channels and all");
    }
    std::uint64_t Revision() const {
        std::uint64_t result = 0; Check(history.Revision(result) == MMS_OK, "camera interpolation revision readable"); return result;
    }
    void RevisionMatches() const {
        MmsSceneQueryV1 query{}; query.h = plugins::Header<MmsSceneQueryV1>(); query.flags = MMS_SCENE_CURRENT_TIME;
        MmsSnapshotV1 snapshot{}; Check(scene.acquire(scene.context, &query, &snapshot) == MMS_OK, "camera interpolation scene revision readable");
        const auto revision = snapshot.sceneRevision; scene.release(scene.context, snapshot.handle);
        Check(revision == Revision(), "camera commands and actual edit DLL expose one scene revision");
    }
    Bytes ViewAndOtherAuthors() const {
        Bytes b;
        b.Add(app.CameraMode()); b.Add(app.SelectedModelSlot()); b.Add(app.SelectedAccessorySlot());
        b.Add(app.MainModelComboSelection()); b.Add(app.CurrentFrame()); b.Add(app.LastRegisteredFrame());
        b.Add(app.CameraTrackCursor()); b.Add(app.CameraTrackActive());
        b.Add(app.LightTrackCursor()); b.Add(app.LightTrackActive()); b.Add(app.ShadowTrackCursor()); b.Add(app.ShadowTrackActive());
        b.Add(app.state.globalTrackSelected, sizeof app.state.globalTrackSelected);
        for (unsigned axis = 0; axis < 3; ++axis) {
            b.Add(app.CameraPositionAxis(axis)); b.Add(app.CameraRotation()[axis]);
            b.Add(app.LightDirection()[axis]); b.Add(app.LightColor()[axis]);
        }
        b.Add(app.CameraDistance()); b.Add(app.CameraFov()); b.Add(app.CameraPerspective());
        b.Add(app.CameraParentModel()); b.Add(app.CameraParentBone()); b.Add(app.ShadowMode()); b.Add(app.ShadowDistance());
        b.Add(Presets(app)); b.Add(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        for (int control : {436, 471, panel::kRegisterScopeCombo})
            b.Add(SendDlgItemMessageW(app.MainWindow(), control, CB_GETCURSEL, 0, 0));
        for (std::size_t slot = 0; slot < mdl::KeyPoolCapacity(app.CameraKeys()); ++slot) b.Add(app.CameraKeys()[slot].selected);
        b.Pool(app.LightKeys()); b.Pool(app.ShadowKeys()); b.Pool(app.GravityKeys());
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto& model = *mdl::Mdl(app.ModelSlot(slot));
            b.Add(model.undoRings, sizeof model.undoRings); b.Add(model.undoState); b.Add(model.undoDirty); b.Add(model.redoDirty);
            b.Add(model.selectedBone); b.Add(model.selectedMorphs); b.Add(model.boneSelection, model.boneCount);
            b.Add(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys) * sizeof(mdl::BoneKey));
            b.Add(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys) * sizeof(mdl::MorphKey));
            b.Add(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys) * sizeof(mdl::DisplayKey));
            for (unsigned bone = 0; bone < model.boneCount; ++bone) {
                b.Add(model.boneTable[bone].trans); b.Add(model.boneTable[bone].rotQuat); b.Add(model.bonePhysicsState[bone]);
            }
            for (unsigned morph = 0; morph < model.morphCount; ++morph) b.Add(model.morphs[morph].value);
        }
        for (unsigned slot = 0; slot < 2; ++slot) {
            b.Pool(app.AccessoryKeys(slot)); const auto& accessory = *app.AccessorySlot(slot);
            b.Add(accessory.position); b.Add(accessory.rotation); b.Add(accessory.scale); b.Add(accessory.opacity);
            b.Add(accessory.parentModel); b.Add(accessory.parentBone); b.Add(accessory.visible); b.Add(accessory.rowSelected);
        }
        return b;
    }
    Bytes EntireState() const {
        auto b = ViewAndOtherAuthors(); b.Pool(app.CameraKeys()); b.Add(app.SceneModified());
        b.Add(app.RecordedFrameCountByte1()); b.Add(app.PhysicsResetPending());
        b.Add(history.UndoCount()); b.Add(history.RedoCount()); b.Add(Revision()); b.Add(pluginValue()); b.Add(pending()); return b;
    }
    void Stable(const Bytes& before) const {
        Check(app.CameraKeys() == pool && mdl::KeyPoolCapacity(pool) == capacity && mdl::KeyPoolIdentity(pool) == identity,
              "camera interpolation/history preserves native key storage and identity");
        const auto after = ViewAndOtherAuthors();
        if (before.data != after.data) {
            std::size_t at = 0; while (at < before.data.size() && at < after.data.size() && before.data[at] == after.data[at]) ++at;
            std::printf("DIFF camera view/other authors offset=%zu before=%zu after=%zu\n", at, before.data.size(), after.data.size());
        }
        Check(before.data == after.data,
              "camera interpolation/history keeps current pose playhead marks row selection metadata and unrelated dirty authors");
        RevisionMatches();
    }
    void Command(UINT command) {
        const auto before = ViewAndOtherAuthors(); SendMessageW(app.MainWindow(), WM_COMMAND, command, 0); Stable(before);
    }
    void Plugin(const wchar_t* label) {
        const auto before = ViewAndOtherAuthors(); edit_history_host_checks::Menu(&app, label);
        edit_history_host_checks::FlushObjectUi(&app); plugins::Refresh(&app); Stable(before);
    }
    void Channel(int index) {
        SendMessageW(combo, CB_SETCURSEL, index, 0);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == index, "actual camera interpolation channel selected");
    }
    void NoMutation(UINT command, const char* message) {
        app.SceneModified() = 0; const auto before = EntireState(); Command(command);
        Check(before.data == EntireState().data, message);
    }
};

void ChannelsAndHistory(Context& c) {
    Curves pasted{}, linear{};
    for (unsigned lane = 0; lane < 6; ++lane) {
        pasted[lane] = {static_cast<unsigned char>(25 + lane), static_cast<unsigned char>(37 + lane),
                        static_cast<unsigned char>(88 + lane), static_cast<unsigned char>(110 + lane)};
        linear[lane] = {20, 20, 107, 107};
    }
    for (int channel = 0; channel <= 6; ++channel) {
        std::printf("RUN real camera interpolation channel=%d\n", channel); std::fflush(stdout);
        c.Channel(channel); Mark(c.app); Presets(c.app, pasted);
        const auto original = Keys(c.app), applied = Changed(original, channel, pasted), reset = Changed(applied, channel, linear);
        const auto revision = c.Revision(); Check(!c.history.UndoCount() && c.pluginValue() == 0, "camera round starts at original author state");
        c.Command(431);
        SameKeys(c.app, applied, "actual 431 writes only selected camera keys and selected interpolation channels");
        Check(c.history.UndoCount() == 1 && !c.history.RedoCount() && c.Revision() == revision + 1 && c.app.SceneModified(),
              "one real camera paste adds exactly one journal node and revision");
        c.NoMutation(431, "same-value camera paste is a no-op without dirty or revision changes");

        // Selection and the playhead may change before history navigation. The
        // authoring record must neither restore old marks nor seek the camera.
        Mark(c.app, true); c.app.CurrentFrame() = 17 + channel; c.app.CameraPositionX() = 123.25f + channel;
        c.Command(400); SameKeys(c.app, original, "native undo restores camera interpolation without overwriting current marks");
        Check(!c.history.UndoCount() && c.history.RedoCount() == 1 && c.Revision() == revision + 2,
              "one native undo reverses the whole selected camera batch");
        Mark(c.app); Curves old{};
        for (unsigned lane = 0; lane < 6; ++lane)
            for (unsigned point = 0; point < 4; ++point) old[lane][point] = original[0].interpolation[point][lane];
        Presets(c.app, old); c.NoMutation(431, "same-data camera paste preserves an existing redo branch");
        for (std::size_t slot = 0; slot < c.capacity; ++slot) c.app.CameraKeys()[slot].selected = 0;
        c.NoMutation(431, "zero camera selection preserves redo on paste");
        c.NoMutation(432, "zero camera selection preserves redo on reset");
        Mark(c.app); Presets(c.app, pasted); c.Plugin(L"Edit probe redo");
        SameKeys(c.app, applied, "real plugin redo restores the camera paste at a changed playhead");
        Check(c.history.UndoCount() == 1 && !c.history.RedoCount() && c.Revision() == revision + 3,
              "plugin redo and native camera command use one journal revision");
        c.Command(432); SameKeys(c.app, reset, "actual 432 resets only the selected camera interpolation channels");
        Check(c.history.UndoCount() == 2 && c.Revision() == revision + 4, "camera reset adds one shared node");
        c.NoMutation(432, "repeating a camera interpolation reset creates no extra history");

        if (channel == 6) {
            c.Plugin(L"Edit probe once"); Check(c.pluginValue() == 1 && c.history.UndoCount() == 3, "real DLL edit follows native camera interpolation in one journal");
            c.Command(400); Check(c.pluginValue() == 0 && c.history.UndoCount() == 2, "native undo reverses the real DLL edit first");
            SameKeys(c.app, reset, "undoing plugin state does not change camera interpolation");
            c.Plugin(L"Edit probe undo"); SameKeys(c.app, applied, "plugin undo next reverses the whole camera reset");
            c.Plugin(L"Edit probe redo"); SameKeys(c.app, reset, "plugin redo restores the camera reset");
            c.Command(401); Check(c.pluginValue() == 1 && c.history.UndoCount() == 3, "native redo next restores real plugin state");
            c.Plugin(L"Edit probe undo"); Check(c.pluginValue() == 0, "plugin undo restores its own state before final camera cleanup");
        }
        c.Plugin(L"Edit probe undo"); SameKeys(c.app, applied, "plugin undo restores camera data before reset");
        c.Command(400); SameKeys(c.app, original, "native undo restores the original camera interpolation for the next channel");
        Check(!c.history.UndoCount() && c.history.RedoCount() >= 2, "camera round leaves a real redo branch for subsequent rejection checks");
    }
}

void Rejections(Context& c) {
    std::puts("RUN actual camera interpolation rejection paths"); std::fflush(stdout);
    Mark(c.app); c.Channel(0);
    for (UINT command : {431u, 432u}) {
        SendMessageW(c.combo, CB_SETCURSEL, WPARAM(-1), 0);
        c.NoMutation(command, "CB_ERR camera interpolation selector consumes command without legacy fallback");
        const auto extra = SendMessageW(c.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Invalid probe item"));
        Check(extra == 7, "test installs one out-of-range camera channel after the real all item");
        c.Channel(7); c.NoMutation(command, "out-of-range camera channel never indexes the preset arrays");
        SendMessageW(c.combo, CB_DELETESTRING, extra, 0); c.Channel(0);
    }
    const auto rejected = [&](const char* reason) {
        std::printf("RUN camera interpolation blocked by %s\n", reason); std::fflush(stdout);
        c.NoMutation(431, "blocked camera paste preserves keys pose marks dirty revision and redo");
        c.NoMutation(432, "blocked camera reset preserves keys pose marks dirty revision and redo");
    };
    c.app.PlaybackActive() = 1; rejected("playback"); c.app.PlaybackActive() = 0;
    c.app.FrameStepPlayback() = 1; rejected("frame step"); c.app.FrameStepPlayback() = 0;
    c.app.state.recordPlaybackStartPending = true; rejected("pending recording"); c.app.state.recordPlaybackStartPending = false;
    c.Plugin(L"Edit probe pending"); Check(c.pending(), "real DLL begins an unprepared pending edit");
    rejected("pending plugin edit"); c.Plugin(L"Edit probe cancel"); Check(!c.pending(), "real DLL cancels pending edit");
    c.Plugin(L"Edit probe prepared"); Check(c.pending(), "real DLL retains a prepared edit");
    rejected("prepared plugin edit"); c.Plugin(L"Edit probe cancel"); Check(!c.pending(), "real DLL cancels prepared edit");
    Check(!c.history.UndoCount() && c.history.RedoCount() >= 2 && !c.pluginValue(),
          "all blocked camera requests preserve the existing redo branch and plugin state");
}

void Run(MMDApp* app) {
    using namespace plugins;
    const auto initial = GetModuleHandleW(L"manager_edit_probe.dll");
    const auto initialFlags = reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(initial, "GetProbeFlags"));
    Check(initial && initialFlags && (initialFlags() & 2) && !(initialFlags() & 1), "ordinary camera host keeps mms.edit unavailable");
    wchar_t model[256]{}, accessory[256]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", model, 256) &&
          GetEnvironmentVariableW(L"MMS_SCENE_TEST_ACCESSORY", accessory, 256), "camera interpolation original fixtures supplied");
    LoadModelFile(app, model, false); LoadModelFile(app, model, false); LoadAccessoryFile(accessory); LoadAccessoryFile(accessory);
    Check(app->ModelSlot(0) && app->ModelSlot(1) && app->AccessorySlot(0) && app->AccessorySlot(1), "camera probe loads two real models and accessories before binding");
    auto* keys = app->CameraKeys(); Check(keys && mdl::KeyPoolIdentity(keys) && mdl::KeyPoolCapacity(keys) > 4, "real host owns a sufficiently sized camera pool");
    const auto root = keys[0];
    for (unsigned slot = 0; slot < 4; ++slot) {
        keys[slot] = root; keys[slot].frame = slot * 10; keys[slot].previous = slot ? slot - 1 : 0; keys[slot].next = slot < 3 ? slot + 1 : 0;
        keys[slot].distance = -24.f - slot; keys[slot].eye[0] = float(slot * 3); keys[slot].target[1] = float(slot) * .125f;
        keys[slot].parentModel = 1; keys[slot].parentBone = 0;
        for (unsigned lane = 0; lane < 6; ++lane) {
            keys[slot].interpolation[0][lane] = static_cast<unsigned char>(12 + lane + (slot == 2 ? 1 : 0));
            keys[slot].interpolation[1][lane] = static_cast<unsigned char>(26 + lane);
            keys[slot].interpolation[2][lane] = static_cast<unsigned char>(76 + lane);
            keys[slot].interpolation[3][lane] = static_cast<unsigned char>(98 + lane);
        }
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto& m = *mdl::Mdl(app->ModelSlot(slot)); Check(m.boneCount && m.bonePhysicsState, "real model contains dirty pose storage");
        m.boneTable[0].trans[1] = slot ? 8.5f : 4.25f; m.bonePhysicsState[0] = 1;
        m.boneKeys[0].allocated = 3; m.morphKeys[0].allocated = 4; m.displayKeys[0].allocated = 5;
        app->AccessorySlot(slot)->position[0] = 40.25f + slot; app->AccessorySlot(slot)->rotation[1] = .375f;
        app->AccessorySlot(slot)->rowSelected = slot ? 1 : 0;
    }
    app->LightKeys()[0].selected = 3; app->ShadowKeys()[0].selected = 4; app->GravityKeys()[0].selected = 5;
    app->AccessoryKeys(0)[0].selected = 6; app->AccessoryKeys(1)[0].selected = 7;
    app->CameraMode() = 1; app->MainModelComboSelection() = 0; app->SelectedModelSlot() = 0;
    app->SelectedAccessorySlot() = 0; app->CurrentFrame() = 7; app->LastRegisteredFrame() = 30;
    SendDlgItemMessageW(app->MainWindow(), 436, CB_SETCURSEL, 0, 0);
    SendDlgItemMessageW(app->MainWindow(), 471, CB_SETCURSEL, 0, 0);
    app->CameraTrackCursor() = 0; app->CameraTrackActive() = 1;
    app->CameraPositionX() = 99.25f; app->CameraRotation()[1] = .625f; app->CameraDistance() = -47.5f;
    app->CameraParentModel() = 1; app->CameraParentBone() = 0;
    for (unsigned lane = 0; lane < 4; ++lane) app->state.globalTrackSelected[lane] = static_cast<unsigned char>(lane & 1);
    EnsureInterpChannelCombo(app, true); Mark(*app);
    Shutdown(); Check(InitializeHistoryTestRuntime(app), "explicit camera probe runtime attaches native/plugin common history");
    auto* history = NativeHistory(app); Check(history && BoundNativePoseHistory(app) == history, "camera controlled history binding exists");
    const auto library = GetModuleHandleW(L"manager_edit_probe.dll");
    const auto value = reinterpret_cast<int(*)()>(GetProcAddress(library, "GetProbeValue"));
    const auto flags = reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(library, "GetProbeFlags"));
    const auto pending = reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(library, "GetProbePending"));
    Check(value && flags && pending && (flags() & 1) && !(flags() & 2), "actual camera probe DLL negotiates controlled edit service");
    Check(RefreshBoundObjectUi(*app, *history), "camera fixture synchronizes real object and scope lists before measured commands");
    auto scene = CreateSceneStore(app); SceneClient client; Context c(*app, *history, scene->Api(client), value, pending);
    ChannelsAndHistory(c); Rejections(c);
    std::puts("PASS actual camera 431/432, six channels/all, real DLL mixed undo/redo, no-op and blocked requests");
}
}
