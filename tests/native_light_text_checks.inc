#include <CommCtrl.h>
namespace mikumikustudio { void CommitEditControl(MMDApp*, HWND); }
namespace {
std::array<float, 6> LightTextPose(MMDApp& app) { return {app.LightColor()[0], app.LightColor()[1], app.LightColor()[2],
    app.LightDirection()[0], app.LightDirection()[1], app.LightDirection()[2]}; }
bool LightTextMirror(MMDApp& app) {
    const auto& light = app.SceneLight();
    return light.Type == D3DLIGHT_DIRECTIONAL && light.Ambient.r == app.LightColor()[0] && light.Ambient.g == app.LightColor()[1] &&
        light.Ambient.b == app.LightColor()[2] && light.Specular.r == app.LightColor()[0] && light.Specular.g == app.LightColor()[1] &&
        light.Specular.b == app.LightColor()[2] && light.Direction.x == app.LightDirection()[0] &&
        light.Direction.y == app.LightDirection()[1] && light.Direction.z == app.LightDirection()[2];
}
struct LightTextFixture : PoseNumericFixture {
    LightTextFixture() {
        app->CameraMode() = 1; app->LightKeys() = mdl::KeyPoolCreate<mdl::LightKey>(4);
        for (unsigned i = 0; i < 3; ++i) { app->LightColor()[i] = .25f + float(i) * .1f; app->LightDirection()[i] = .3f + float(i) * .1f; }
        app->ApplyTimelineLightState();
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES}; Check(InitCommonControlsEx(&common), "initialize hidden light sliders");
        for (unsigned field = 0; field < 6; ++field) {
            Check(CreateWindowExA(0, "EDIT", "0", WS_CHILD, 0, 0, 80, 20, app->MainWindow(),
                reinterpret_cast<HMENU>(INT_PTR(461 + field)), GetModuleHandleW(nullptr), nullptr) != nullptr, "create light text control");
            const auto slider = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD, 0, 0, 80, 20, app->MainWindow(),
                reinterpret_cast<HMENU>(INT_PTR(455 + field)), GetModuleHandleW(nullptr), nullptr);
            Check(slider != nullptr, "create hidden light slider"); SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(-10000, 10000));
        }
    }
    ~LightTextFixture() { history.Reset(); history.Collect(); mdl::KeyPoolDestroy(app->LightKeys()); }
    HWND Field(unsigned field) { return GetDlgItem(app->MainWindow(), 461 + int(field)); }
    void TypeField(unsigned field, const char* text) { SetWindowTextA(Field(field), text); SendMessageA(Field(field), EM_SETMODIFY, TRUE, 0); }
    void CommitField(unsigned field, const char* text) { TypeField(field, text); CommitEditControl(app.get(), Field(field)); }
    void LoseField(unsigned field) { EditSubclassProc(Field(field), WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0); }
};
void LightTextRoutingChecks() {
    for (unsigned field = 0; field < 6; ++field) {
        LightTextFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        const auto before = LightTextPose(*f.app); auto target = before;
        target[field] = field < 3 ? .5f : .123456f;
        const auto ring = PoseRingDigest(f.first);
        f.TypeField(field, field < 3 ? "128.375" : ".1234567"); f.LoseField(field);
        Check(LightTextPose(*f.app) == target && LightTextMirror(*f.app) && f.history.UndoCount() == 1 &&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Light), "light text preserves legacy RGB and seven-character direction conversion with coherent CPU mirror");
        Check(SendDlgItemMessageW(f.app->MainWindow(), 455 + int(field), TBM_GETPOS, 0, 0) == (field < 3 ? 128 : 12),
            "light text synchronizes its paired slider after publication");
        f.LoseField(field); CommitEditControl(f.app.get(), f.Field(field));
        Check(LightTextPose(*f.app) == target && f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring,
            "unmodified rounded light echo does not overwrite precise direction or append history");
        f.app->CurrentFrame() = 37;
        Check(f.history.History(false) == MMS_OK && LightTextPose(*f.app) == before && LightTextMirror(*f.app) && f.app->CurrentFrame() == 37,
            "light text undo restores scalar authoring and derived mirror without seeking");
        for (const char* invalid : {"NaN", "inf", "", "3 junk", "1e999"}) {
            f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
            const auto mirror = f.app->SceneLight(); f.CommitField(field, invalid);
            Check(LightTextPose(*f.app) == before && !std::memcmp(&f.app->SceneLight(), &mirror, sizeof(mirror)) &&
                f.history.RedoCount() == 1 && !f.app->SceneModified(), "invalid light text preserves scalar mirror dirty state and redo");
        }
        Check(f.history.History(true) == MMS_OK && LightTextPose(*f.app) == target && LightTextMirror(*f.app),
            "light text redo remains coherent after invalid inputs");
    }
}
void LightTextFailureChecks() {
    for (unsigned field = 0; field < 6; ++field) {
        LightTextFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        f.CommitField(field, field < 3 ? "140" : ".7"); Check(f.history.History(false) == MMS_OK, "seed light text redo");
        const auto before = LightTextPose(*f.app); const auto mirror = f.app->SceneLight(); const auto ring = PoseRingDigest(f.first);
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        unsigned failures = 0; bool succeeded = false;
        for (long limit = 0; limit < 128; ++limit) {
            f.TypeField(field, field < 3 ? "128" : ".1234567"); failAfter = limit; CommitEditControl(f.app.get(), f.Field(field)); failAfter = -1;
            if (f.history.UndoCount()) { succeeded = true; break; }
            ++failures;
            Check(LightTextPose(*f.app) == before && !std::memcmp(&f.app->SceneLight(), &mirror, sizeof(mirror)) &&
                f.history.RedoCount() == 1 && PoseRingDigest(f.first) == ring && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
                "light text allocation failure preserves all scalar and mirror fields with redo intact");
            Check(f.history.Enter() == MMS_OK, "light text failure releases journal gate"); f.history.Leave();
        }
        Check(succeeded && failures >= 5 && LightTextMirror(*f.app), "light text failure sweep reaches successful atomic publication");
    }
}
}
void NativeLightTextChecks() {
    LightTextRoutingChecks(); LightTextFailureChecks();
    LightTextFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto original = LightTextPose(*f.app); f.CommitField(0, "128");
    const auto capacity = mdl::KeyPoolCapacity(f.app->LightKeys()); auto* grown = mdl::KeyPoolCreate<mdl::LightKey>(capacity + 4);
    std::memcpy(grown, f.app->LightKeys(), capacity * sizeof(mdl::LightKey)); mdl::KeyPoolCommitGrowth(f.app->LightKeys(), grown);
    Check(f.history.History(false) == MMS_OK && LightTextPose(*f.app) == original && LightTextMirror(*f.app),
        "light text record survives identity-preserving pool growth");
    Check(f.history.History(true) == MMS_OK && f.app->LightColor()[0] == .5f, "light text redo survives pool growth");
    auto* replacement = mdl::KeyPoolCreate<mdl::LightKey>(capacity + 4);
    mdl::KeyPoolDestroy(f.app->LightKeys()); f.app->LightKeys() = replacement;
    const auto before = LightTextPose(*f.app); const auto mirror = f.app->SceneLight();
    Check(f.history.History(false) == MMS_E_STALE && LightTextPose(*f.app) == before &&
        !std::memcmp(&mirror, &f.app->SceneLight(), sizeof(mirror)) && f.history.UndoCount() == 1,
        "replaced light scene source rejects undo without scalar mirror or cursor changes");
}
