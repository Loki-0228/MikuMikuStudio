#include "mikudancestudio/clipboard_layout.hpp"
namespace key_paste_host_checks {
void Run(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    auto* oldClipboard = app->BoneClipboard(); const auto oldCounts = app->ClipboardCounts();
    const auto mode = app->CameraMode(); const auto frame = app->CurrentFrame(); const auto slot = app->SelectedModelSlot();
    const auto recordingMode = history.Mode();
    std::array<unsigned char, sizeof(model.undoRings)> rings{};
    std::memcpy(rings.data(), model.undoRings, rings.size());
    const auto undoState = model.undoState[0]; const auto redoState = model.undoState[1];
    const std::vector<mdl::BoneKey> before(model.boneKeys, model.boneKeys + mdl::BoneKeyCapacity(model.boneKeys));
    mdl::BoneClipboardRecord clipboard{};
    std::memcpy(clipboard.name, model.boneTable[0].name, sizeof(model.boneTable[0].name));
    clipboard.rotation[3] = 1; clipboard.position[1] = 11.25f;
    for (int lane = 0; lane < 4; ++lane) {
        clipboard.interpolation[lane] = clipboard.interpolation[lane + 4] = 20;
        clipboard.interpolation[lane + 8] = clipboard.interpolation[lane + 12] = 107;
    }
    app->CameraMode() = 0; app->SelectedModelSlot() = 0; app->CurrentFrame() = 317;
    app->BoneClipboard() = &clipboard; app->ClipboardCounts() = {}; app->ClipboardCounts().bones = 1;
    const auto command = [&](UINT id) { SendMessageW(app->MainWindow(), WM_COMMAND, id, 0); };
    for (auto recording : {plugins::SharedHistory::RecordingMode::AllOperations, plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        Check(history.SetMode(recording) == MMS_OK, "actual paste history mode configured");
        Menu(app, L"Edit probe prepared"); command(421);
        Check(!history.UndoCount() && !std::memcmp(model.boneKeys, before.data(), before.size() * sizeof(mdl::BoneKey)),
            "pending plugin transaction blocks real paste before native write");
        Menu(app, L"Edit probe cancel"); Menu(app, L"Edit probe once"); command(421);
        Check(history.UndoCount() == 2 && value() == 1, "actual paste joins plugin history as one explicit node in either mode");
        unsigned target = 0; const auto capacity = mdl::BoneKeyCapacity(model.boneKeys);
        while (model.boneKeys[target].frame < 317 && model.boneKeys[target].next) {
            target = model.boneKeys[target].next; Check(target < capacity, "actual pasted chain remains bounded");
        }
        Check(model.boneKeys[target].frame == 317, "real command publishes clipboard key at current time");
        const auto pasted = model.boneKeys[target]; clipboard.position[1] = 77;
        Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
        Menu(app, L"Edit probe undo");
        Check(!std::memcmp(model.boneKeys, before.data(), before.size() * sizeof(mdl::BoneKey)) && value() == 1,
            "real paste undo restores keys between plugin edits");
        app->CameraMode() = 1; app->CurrentFrame() = frame + 91; app->SelectedModelSlot() = 1;
        Menu(app, L"Edit probe redo");
        Check(!std::memcmp(&model.boneKeys[target], &pasted, sizeof(pasted)) && app->CurrentFrame() == frame + 91 && value() == 1,
            "real paste redo owns clipboard bytes and target across selector and time changes");
        Menu(app, L"Edit probe redo");
        Check(value() == 2 && model.undoState[0] == undoState && model.undoState[1] == redoState &&
            !std::memcmp(model.undoRings, rings.data(), rings.size()), "real paste and plugin history leave legacy rings unchanged");
        for (int i = 0; i < 3; ++i) Menu(app, L"Edit probe undo");
        Check(value() == 0 && history.Reset() == MMS_OK, "real paste probe restores isolated journal");
        app->CameraMode() = 0; app->SelectedModelSlot() = 0; app->CurrentFrame() = 317; clipboard.position[1] = 11.25f;
    }
    Check(history.SetMode(recordingMode) == MMS_OK, "restore host recording mode after paste checks");
    app->BoneClipboard() = oldClipboard; app->ClipboardCounts() = oldCounts;
    app->CameraMode() = mode; app->CurrentFrame() = frame; app->SelectedModelSlot() = slot;
    std::puts("PASS actual bone paste, clipboard independence, history modes and plugin interleaving");
}
}
